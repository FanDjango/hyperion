/* IMPL.C       (c) Copyright Roger Bowler, 1999-2012                */
/*              Hercules Initialization Module                       */
/*                                                                   */
/*   Released under "The Q Public License Version 1"                 */
/*   (http://www.hercules-390.org/herclic.html) as modifications to  */
/*   Hercules.                                                       */

/*-------------------------------------------------------------------*/
/* This module initializes the Hercules S/370 or ESA/390 emulator.   */
/* It builds the system configuration blocks, creates threads for    */
/* central processors, HTTP server, logger task and activates the    */
/* control panel which runs under the main thread when in foreground */
/* mode.                                                             */
/*-------------------------------------------------------------------*/

#include "hstdinc.h"

#ifndef _IMPL_C_
#define _IMPL_C_
#endif

#ifndef _HENGINE_DLL_
#define _HENGINE_DLL_
#endif

#include "hercules.h"
#include "opcode.h"
#include "devtype.h"
#include "herc_getopt.h"
#include "hostinfo.h"
#include "history.h"
#include "hRexx.h"
#if defined( OPTION_W32_CTCI )      // (need tt32_get_default_iface)
#include "w32ctca.h"
#endif

static char shortopts[] =

#if defined( EXTERNALGUI )
    "e"
#endif
   "h::f:r:db:vt::"
#if defined(ENABLE_BUILTIN_SYMBOLS)
   "s:"
#endif
#if defined(OPTION_DYNAMIC_LOAD)
   "p:l:"
#endif
   ;

#if defined(HAVE_GETOPT_LONG)
static struct option longopts[] =
{
    { "test",     optional_argument, NULL, 't' },
    { "help",     optional_argument, NULL, 'h' },
    { "config",   required_argument, NULL, 'f' },
    { "rcfile",   required_argument, NULL, 'r' },
    { "daemon",   no_argument,       NULL, 'd' },
    { "herclogo", required_argument, NULL, 'b' },
    { "verbose",  no_argument,       NULL, 'v' },
#if defined( EXTERNALGUI )
    { "externalgui",  no_argument,   NULL, 'e' },
#endif

#if defined(ENABLE_BUILTIN_SYMBOLS)
    { "defsym",   required_argument, NULL, 's' },
#endif

#if defined(OPTION_DYNAMIC_LOAD)
    { "modpath",  required_argument, NULL, 'p' },
    { "ldmod",    required_argument, NULL, 'l' },
#endif
    { NULL,       0,                 NULL,  0  }
};
#endif

static LOGCALLBACK  log_callback = NULL;

struct cfgandrcfile
{
   const char * filename;             /* Or NULL                     */
   const char * const envname;        /* Name of environ var to test */
   const char * const defaultfile;    /* Default file                */
   const char * const whatfile;       /* config/restart, for message */
};

enum cfgorrc
{
   want_cfg,
   want_rc,
   cfgorrccount
};

static struct cfgandrcfile cfgorrc[ cfgorrccount ] =
{
   { NULL, "HERCULES_CNF", "hercules.cnf", "Configuration", },
   { NULL, "HERCULES_RC",  "hercules.rc",  "Run Commands",  },
};

#if defined(OPTION_DYNAMIC_LOAD)
#define MAX_DLL_TO_LOAD         50
static char   *dll_load[MAX_DLL_TO_LOAD];   /* Pointers to modnames  */
static int     dll_count = -1;              /* index into array      */
#endif

/* forward define process_script_file (ISW20030220-3) */
extern int process_script_file(char *,int);
/* extern int quit_cmd(int argc, char *argv[],char *cmdline);        */

/* Forward declarations:                                             */
static void init_progname( int argc, char* argv[] );
static int process_args( int argc, char* argv[] );
/* End of forward declarations.                                      */

/*-------------------------------------------------------------------*/
/* Register a LOG callback                                           */
/*-------------------------------------------------------------------*/
DLL_EXPORT void registerLogCallback( LOGCALLBACK cb )
{
    log_callback = cb;
}

/*-------------------------------------------------------------------*/
/* Subroutine to exit process after flushing stderr and stdout       */
/*-------------------------------------------------------------------*/
static void delayed_exit (int exit_code)
{
    UNREFERENCED(exit_code);

    /* Delay exiting is to give the system
     * time to display the error message. */
#if defined( _MSVC_ )
    SetConsoleCtrlHandler( NULL, FALSE); // disable Ctrl-C intercept
#endif
    sysblk.shutimmed = TRUE;

    fflush(stderr);
    fflush(stdout);
    usleep(100000);
    do_shutdown();
    fflush(stderr);
    fflush(stdout);
    usleep(100000);
    return;
}

/*-------------------------------------------------------------------*/
/* Signal handler for SIGINT signal                                  */
/*-------------------------------------------------------------------*/
static void sigint_handler (int signo)
{
//  logmsg ("impl.c: sigint handler entered for thread %lu\n",/*debug*/
//          thread_id());                                     /*debug*/

    UNREFERENCED(signo);

    signal(SIGINT, sigint_handler);
    /* Ignore signal unless presented on console thread */
    if ( !equal_threads( thread_id(), sysblk.cnsltid ) )
        return;

    /* Exit if previous SIGINT request was not actioned */
    if (sysblk.sigintreq)
    {
        /* Release the configuration */
        release_config();
        delayed_exit(1);
    }

    /* Set SIGINT request pending flag */
    sysblk.sigintreq = 1;

    /* Activate instruction stepping */
    sysblk.inststep = 1;
    SET_IC_TRACE;
    return;
} /* end function sigint_handler */

/*-------------------------------------------------------------------*/
/* Signal handler for SIGTERM signal                                 */
/*-------------------------------------------------------------------*/
static void sigterm_handler (int signo)
{
//  logmsg ("impl.c: sigterm handler entered for thread %lu\n",/*debug*/
//          thread_id());                                      /*debug*/

    UNREFERENCED(signo);

    signal(SIGTERM, sigterm_handler);
    /* Ignore signal unless presented on main program (impl) thread */
    if ( !equal_threads( thread_id(), sysblk.impltid ) )
        return;

    /* Initiate system shutdown */
    do_shutdown();

    return;
} /* end function sigterm_handler */

#if defined( _MSVC_ )
/*-------------------------------------------------------------------*/
/* Perform immediate/emergency shutdown                              */
/*-------------------------------------------------------------------*/
static void do_emergency_shutdown()
{
    sysblk.shutdown = TRUE;

    if (!sysblk.shutimmed)
    {
        sysblk.shutimmed = TRUE;
        do_shutdown();
    }
    else // (already in progress)
    {
        while (!sysblk.shutfini)
            usleep(100000);
    }
}

/*-------------------------------------------------------------------*/
/* Windows console control signal handler                            */
/*-------------------------------------------------------------------*/
static BOOL WINAPI console_ctrl_handler( DWORD signo )
{
    switch ( signo )
    {
    ///////////////////////////////////////////////////////////////
    //
    //                    PROGRAMMING NOTE
    //
    ///////////////////////////////////////////////////////////////
    //
    // "SetConsoleCtrlHandler function HandlerRoutine Callback
    //  Function:"
    //
    //   "Return Value:"
    //
    //     "If the function handles the control signal, it
    //      should return TRUE."
    //
    //   "CTRL_LOGOFF_EVENT:"
    //
    //     "Note that this signal is received only by services.
    //      Interactive applications are terminated at logoff,
    //      so they are not present when the system sends this
    //      signal."
    //
    //   "CTRL_SHUTDOWN_EVENT:"
    //
    //     "Interactive applications are not present by the time
    //      the system sends this signal, therefore it can be
    //      received only be services in this situation."
    //
    //   "CTRL_CLOSE_EVENT:"
    //
    //      "... if the process does not respond within a certain
    //       time-out period (5 seconds for CTRL_CLOSE_EVENT..."
    //
    ///////////////////////////////////////////////////////////////
    //
    //  What this all boils down to is we'll never receive the
    //  logoff and shutdown signals (via this callback), and
    //  we only have a maximum of 5 seconds to return TRUE from
    //  the CTRL_CLOSE_EVENT signal. Thus, as normal shutdowns
    //  may likely take longer than 5 seconds and our goal is
    //  to try hard to shutdown Hercules as gracfully as we can,
    //  we are left with little choice but to always perform
    //  an immediate/emergency shutdown for CTRL_CLOSE_EVENT.
    //
    ///////////////////////////////////////////////////////////////

        case CTRL_BREAK_EVENT:

            // "CTRL_BREAK_EVENT received: %s"
            WRMSG( HHC01400, "I", "pressing interrupt key" );

            OBTAIN_INTLOCK( NULL );
            ON_IC_INTKEY;
            WAKEUP_CPUS_MASK( sysblk.waiting_mask );
            RELEASE_INTLOCK( NULL );

            return TRUE;

        case CTRL_C_EVENT:

            if (!sysblk.shutimmed)
                // "CTRL_C_EVENT received: %s"
                WRMSG( HHC01401, "I", "initiating emergency shutdown" );
            do_emergency_shutdown();
            return TRUE;


        case CTRL_CLOSE_EVENT:

            if (!sysblk.shutimmed)
                // "CTRL_CLOSE_EVENT received: %s"
                WRMSG( HHC01402, "I", "initiating emergency shutdown" );
            do_emergency_shutdown();
            return TRUE;

        default:

            return FALSE;  // (not handled; call next signal handler)
    }

    UNREACHABLE_CODE( return FALSE );
}

/*-------------------------------------------------------------------*/
/* Windows hidden window message handler                             */
/*-------------------------------------------------------------------*/
static LRESULT CALLBACK MainWndProc( HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam )
{
    switch (msg)
    {
    ///////////////////////////////////////////////////////////////
    //
    //                    PROGRAMMING NOTE
    //
    ///////////////////////////////////////////////////////////////
    //
    //  "If an application returns FALSE in response to
    //   WM_QUERYENDSESSION, it still appears in the shutdown
    //   UI. Note that the system does not allow console
    //   applications or applications without a visible window
    //   to cancel shutdown. These applications are automatically
    //   terminated if they do not respond to WM_QUERYENDSESSION
    //   or WM_ENDSESSION within 5 seconds or if they return FALSE
    //   in response to WM_QUERYENDSESSION."
    //
    ///////////////////////////////////////////////////////////////
    //
    //  What this all boils down to is we can NEVER prevent the
    //  user from logging off or shutting down since not only are
    //  we a console application but our window is created invisible
    //  as well. Thus we only have a maximum of 5 seconds to return
    //  TRUE from WM_QUERYENDSESSION or return 0 from WM_ENDSESSION,
    //  and since a normal shutdown may likely take longer than 5
    //  seconds and our goal is to try hard to shutdown Hercules as
    //  gracfully as possible, we are left with little choice but to
    //  perform an immediate emergency shutdown once we receive the
    //  WM_ENDSESSION message with a WPARAM value of TRUE.
    //
    ///////////////////////////////////////////////////////////////

        case WM_QUERYENDSESSION:

            // "%s received: %s"
            WRMSG( HHC01403, "I", "WM_QUERYENDSESSION", "allow" );
            return TRUE;    // Vote "YES"... (we have no choice!)

        case WM_ENDSESSION:

            if (!wParam)    // FALSE? (session not really ending?)
            {
                // Some other application (or the user themselves)
                // has aborted the logoff or system shutdown...

                // "%s received: %s"
                WRMSG( HHC01403, "I", "WM_ENDSESSION", "aborted" );
                return 0; // (message processed)
            }

            // User is logging off or the system is being shutdown.
            // We have a maximum of 5 seconds to shutdown Hercules.

            // "%s received: %s"
            WRMSG( HHC01403, "I", "WM_ENDSESSION", "initiating emergency shutdown" );
            do_emergency_shutdown();
            return 0; // (message handled)

        default:

            return DefWindowProc( hWnd, msg, wParam, lParam );
    }

    UNREACHABLE_CODE( return 0 );
}

// Create invisible message handling window...

HANDLE  g_hWndEvt  = NULL;  // (temporary window creation event)
HWND    g_hMsgWnd  = NULL;  // (window handle of message window)

static void* WinMsgThread( void* arg )
{
    WNDCLASS  wc    =  {0};

    UNREFERENCED( arg );

    wc.lpfnWndProc    =  MainWndProc;
    wc.hInstance      =  GetModuleHandle(0);
    wc.lpszClassName  =  "Hercules";

    RegisterClass( &wc );

    g_hMsgWnd = CreateWindowEx( 0,
        "Hercules", "Hercules",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT,
        NULL, NULL,
        GetModuleHandle(0), NULL );

    SetEvent( g_hWndEvt );      // (indicate create window completed)

    if (g_hMsgWnd)  // (pump messages if window successfully created)
    {
        MSG msg;
        while (GetMessage( &msg, NULL , 0 , 0 ))
        {
            TranslateMessage ( &msg );
            DispatchMessage  ( &msg );
        }
    }
    return NULL;
}
#endif /* defined( _MSVC_ ) */

#if !defined( NO_SIGABEND_HANDLER )
/*-------------------------------------------------------------------*/
/* Linux watchdog thread -- detects malfunctioning CPU engines       */
/*-------------------------------------------------------------------*/
static void* watchdog_thread( void* arg )
{
    REGS* regs;
    S64   savecount[ MAX_CPU_ENGINES ];
    int   cpu;

    UNREFERENCED( arg );

    /* Set watchdog priority just below cpu priority
       such that it will not invalidly detect an
       inoperable cpu
    */
    if (sysblk.cpuprio >= 0)
        set_thread_priority( 0, sysblk.cpuprio + 1 );

    for (cpu=0; cpu < sysblk.maxcpu; cpu++)
        savecount[ cpu ] = -1;

    while (!sysblk.shutdown)
    {
        for (cpu=0; cpu < sysblk.maxcpu; cpu++)
        {
            /* We're only interested in CPUs
               that are ONLINE and STARTED
            */
            if (0
                || !IS_CPU_ONLINE( cpu )
                || (regs = sysblk.regs[ cpu ])->cpustate != CPUSTATE_STARTED
            )
            {
                /* CPU not ONLINE or not STARTED */
                savecount[ cpu ] = -1;
                continue;
            }

            /* CPU is ONLINE and STARTED. Now check to see if it's
               maybe in a WAITSTATE. If so, we're not interested.
            */
            if (0
                || WAITSTATE( &regs->psw )
#if defined( _FEATURE_WAITSTATE_ASSIST )
                || (1
                    && regs->sie_active
                    && WAITSTATE( &regs->guestregs->psw )
                   )
#endif
            )
            {
                /* CPU is in a WAITSTATE */
                savecount[ cpu ] = -1;
                continue;
            }

            /* We have found a running CPU that should be executing
               instructions. Compare its current instruction count
               with our previously saved value. If they're different
               then it has obviously executed SOME instructions and
               all is well. Save its current instruction counter and
               move on the next CPU. This one appears to be healthy.
            */
            if (INSTCOUNT( regs ) != (U64) savecount[ cpu ])
            {
                /* Save updated instruction count for next time */
                savecount[ cpu ] = INSTCOUNT( regs );
                continue;
            }
               
            /*
               Uh oh! We have found a malfunctioning CPU! It has not
               executed any instructions at all within the last check
               interval! Let the defined HDL watchdog hook deal with
               this situation (if such a hook has been defined).
            */
            if (HDC1( debug_watchdog_signal, regs ))
                continue;

            /* Either there was no HDL watchdog hook defined for this
               situation or else it returned false indicating it was
               not abe to deal with the situation itself, so we have
               to deal with it as best we can: send a signal to the
               malfunctioning CPU's thread to cause a Machine Check.
               The guest will then (if it's able to!) deal with the
               its Machine Check however it needs to (e.g. by taking
               the malfunctioning CPU offline, etc).
            */
            signal_thread( sysblk.cputid[ cpu ], SIGUSR1 );

            /* Now reset our saved instruction count value to prevent
               us from throwing another Machine Checks the next time
               we wakeup. This means the guest has one check interval
               to take the CPU offline. (It obviously cannot FIX the
               problem since it's not ITS problem to fix! It's OURS!
               Hercules obviously has a SERIOUS BUG somewhere!)
            */
            savecount[ cpu ] = -1;
        }

        SLEEP( WATCHDOG_SECS );     // (sleep for another interval)

    } // while (!sysblk.shutdown)

    return NULL;
}
#endif /* !defined( NO_SIGABEND_HANDLER ) */

/*-------------------------------------------------------------------*/
/* Herclin (plain line mode Hercules) message callback function      */
/*-------------------------------------------------------------------*/
void* log_do_callback( void* dummy )
{
    char* msgbuf;
    int   msglen;
    int   msgidx = -1;

    UNREFERENCED( dummy );

    while ((msglen = log_read( &msgbuf, &msgidx, LOG_BLOCK )))
        log_callback( msgbuf, msglen );

    /* Let them know logger thread has ended */
    log_callback( NULL, 0 );

    return (NULL);
}

/*-------------------------------------------------------------------*/
/* Return panel command handler to herclin line mode hercules        */
/*-------------------------------------------------------------------*/
DLL_EXPORT  COMMANDHANDLER  getCommandHandler()
{
    return (panel_command);
}

/*-------------------------------------------------------------------*/
/* Process .RC file thread.                                          */
/*                                                                   */
/* Called synchronously when in daemon mode.                         */
/*-------------------------------------------------------------------*/
static void* process_rc_file (void* dummy)
{
    char  pathname[MAX_PATH];

    UNREFERENCED(dummy);

    /* We have a .rc file to run */
    hostpath(pathname, cfgorrc[want_rc].filename, sizeof(pathname));

    /* Wait for panel thread to engage */
// ZZ FIXME:THIS NEEDS TO GO
    if (!sysblk.daemon_mode)
        while (!sysblk.panel_init)
            usleep( 10 * 1000 );

    /* Run the script processor for this file */
    process_script_file(pathname, 1);
        // (else error message already issued)

    return NULL;    /* End the .rc thread */
}

/*-------------------------------------------------------------------*/
/* IMPL main entry point                                             */
/*-------------------------------------------------------------------*/
DLL_EXPORT int impl( int argc, char* argv[] )
{
TID     rctid;                          /* RC file thread identifier */
TID     logcbtid;                       /* RC file thread identifier */
int     rc;

    /* Seed the pseudo-random number generator */
    srand( time(NULL) );

    /* Clear the system configuration block */
    memset( &sysblk, 0, sizeof( SYSBLK ) );

    VERIFY( MLOCK( &sysblk, sizeof( SYSBLK )) == 0);

#if defined (_MSVC_)
    _setmaxstdio(2048);
#endif

    /* Initialize EYE-CATCHERS for SYSBLK */
    memset( &sysblk.blknam, SPACE, sizeof( sysblk.blknam ));
    memset( &sysblk.blkver, SPACE, sizeof( sysblk.blkver ));
    memset( &sysblk.blkend, SPACE, sizeof( sysblk.blkend ));
    sysblk.blkloc = swap_byte_U64( (U64)((uintptr_t) &sysblk ));
    memcpy( sysblk.blknam, HDL_NAME_SYSBLK, strlen( HDL_NAME_SYSBLK ));
    memcpy( sysblk.blkver, HDL_VERS_SYSBLK, strlen( HDL_VERS_SYSBLK ));
    sysblk.blksiz = swap_byte_U32( (U32) sizeof( SYSBLK ));
    {
        char buf[32];
        MSGBUF( buf, "END%13.13s", HDL_NAME_SYSBLK );
        memcpy( sysblk.blkend, buf, sizeof( sysblk.blkend ));
    }
    sysblk.msglvl = DEFAULT_MLVL;

    /* Initialize program name */
    init_progname( argc, argv );

    /* Initialize SETMODE and set user authority */
    SETMODE( INIT );

    SET_THREAD_NAME( "impl" );

    /* Remain compatible with older external gui versions */
#if defined( EXTERNALGUI )
    if (argc >= 1 && strncmp( argv[argc-1], "EXTERNALGUI", 11 ) == 0)
    {
        extgui = TRUE;
        argc--;
    }
#endif

    /* Initialize 'hostinfo' BEFORE display_version is called */
    init_hostinfo( &hostinfo );

#ifdef _MSVC_
    /* Initialize sockets package */
    VERIFY( socket_init() == 0 );
#endif

    /* Ensure hdl_shut is called in case of shutdown
       hdl_shut will ensure entries are only called once */
    atexit( hdl_shut );

#if defined(ENABLE_BUILTIN_SYMBOLS)
    set_symbol( "VERSION", VERSION);
    set_symbol( "BDATE", __DATE__ );
    set_symbol( "BTIME", __TIME__ );

    {
        char num_procs[64];

        if ( hostinfo.num_packages     != 0 &&
             hostinfo.num_physical_cpu != 0 &&
             hostinfo.num_logical_cpu  != 0 )
        {
            MSGBUF( num_procs, "LP=%d, Cores=%d, CPUs=%d", hostinfo.num_logical_cpu,
                                hostinfo.num_physical_cpu, hostinfo.num_packages );
        }
        else
        {
            if ( hostinfo.num_procs > 1 )
                MSGBUF( num_procs, "MP=%d", hostinfo.num_procs );
            else if ( hostinfo.num_procs == 1 )
                STRLCPY( num_procs, "UP" );
            else
                STRLCPY( num_procs, "" );
        }

        set_symbol( "HOSTNAME", hostinfo.nodename );
        set_symbol( "HOSTOS", hostinfo.sysname );
        set_symbol( "HOSTOSREL", hostinfo.release );
        set_symbol( "HOSTOSVER", hostinfo.version );
        set_symbol( "HOSTARCH", hostinfo.machine );
        set_symbol( "HOSTNUMCPUS", num_procs );
    }

    set_symbol( "MODNAME", sysblk.hercules_pgmname );
    set_symbol( "MODPATH", sysblk.hercules_pgmpath );
#endif

    sysblk.sysgroup = DEFAULT_SYSGROUP;

    /* set default console port address */
    sysblk.cnslport = strdup("3270");

    /* set default tape autoinit value to OFF   */
    sysblk.noautoinit = TRUE;

#if defined( OPTION_SHUTDOWN_CONFIRMATION )
    /* set default quit timeout value (also ssd) */
    sysblk.quitmout = QUITTIME_PERIOD;
#endif

    /* Default command separator is OFF (disabled) */
    sysblk.cmdsep = 0;

#if defined(_FEATURE_SYSTEM_CONSOLE)
    /* set default for scpecho to TRUE */
    sysblk.scpecho = TRUE;

    /* set fault for scpimply to FALSE */
    sysblk.scpimply = FALSE;
#endif

    /* set default system state to reset */
    sysblk.sys_reset = TRUE;

    /* Set default SHCMDOPT to DISABLE NODIAG8 */
    sysblk.shcmdopt = 0;

    /* Save process ID */
    sysblk.hercules_pid = getpid();

    /* Save thread ID of main program */
    sysblk.impltid = thread_id();

    /* Save TOD of when we were first IMPL'ed */
    time( &sysblk.impltime );

    /* Set to LPAR mode with LPAR 1, LPAR ID of 01, and CPUIDFMT 0   */
    sysblk.lparmode = 1;                /* LPARNUM 1    # LPAR ID 01 */
    sysblk.lparnum  = 1;                /* ...                       */
    sysblk.cpuidfmt = 0;                /* CPUIDFMT 0                */
    sysblk.operation_mode = om_mif;     /* Default to MIF operaitons */

    /* set default CPU identifier */
    sysblk.cpumodel = 0x0586;
    sysblk.cpuversion = 0xFD;
    sysblk.cpuserial = 0x000001;
    sysblk.cpuid = createCpuId( sysblk.cpumodel, sysblk.cpuversion,
                                sysblk.cpuserial, 0 );

    /* set default Program Interrupt Trace to NONE */
    sysblk.pgminttr = OS_NONE;

    sysblk.timerint = DEF_TOD_UPDATE_USECS;

    /* set default thread priorities */
    sysblk.hercprio = DEFAULT_HERCPRIO;
    sysblk.todprio  = DEFAULT_TOD_PRIO;
    sysblk.cpuprio  = DEFAULT_CPU_PRIO;
    sysblk.devprio  = DEFAULT_DEV_PRIO;
    sysblk.srvprio  = DEFAULT_SRV_PRIO;

    /* Cap the default priorities at zero if setuid not available */
#if !defined( _MSVC_ )
  #if !defined(NO_SETUID)
    if (sysblk.suid)
  #endif
    {
        if (sysblk.hercprio < 0)
            sysblk.hercprio = 0;
        if (sysblk.todprio < 0)
            sysblk.todprio = 0;
        if (sysblk.cpuprio < 0)
            sysblk.cpuprio = 0;
        if (sysblk.devprio < 0)
            sysblk.devprio = 0;
        if (sysblk.srvprio < 0)
            sysblk.srvprio = 0;
    }
#endif

#if defined(_FEATURE_ECPSVM)
    sysblk.ecpsvm.available = 0;
    sysblk.ecpsvm.level = 20;
#endif

#ifdef PANEL_REFRESH_RATE
    sysblk.panrate = PANEL_REFRESH_RATE_SLOW;
#endif

#if defined( OPTION_SHUTDOWN_CONFIRMATION )
    /* Set the quitmout value */
    sysblk.quitmout = QUITTIME_PERIOD;     /* quit timeout value */
#endif

#if defined(OPTION_SHARED_DEVICES)
    sysblk.shrdport = 0;
#endif

#if defined(ENABLE_BUILTIN_SYMBOLS)
    /* setup defaults for CONFIG symbols  */
    {
        char buf[8];

        set_symbol("LPARNAME", str_lparname());
        set_symbol("LPARNUM", "1");
        set_symbol("CPUIDFMT", "0");

        MSGBUF( buf, "%06X", sysblk.cpuserial );
        set_symbol( "CPUSERIAL", buf );

        MSGBUF( buf, "%04X", sysblk.cpumodel );
        set_symbol( "CPUMODEL", buf );

    }
#endif

#if defined( _FEATURE_047_CMPSC_ENH_FACILITY )
    sysblk.zpbits  = DEF_CMPSC_ZP_BITS;
#endif

    /* Initialize locks, conditions, and attributes */
    initialize_lock (&sysblk.config);
    initialize_lock (&sysblk.todlock);
    initialize_lock (&sysblk.mainlock);
    sysblk.mainowner = LOCK_OWNER_NONE;
    initialize_lock (&sysblk.intlock);
    initialize_lock (&sysblk.iointqlk);
    sysblk.intowner = LOCK_OWNER_NONE;
    initialize_lock (&sysblk.sigplock);
    initialize_lock (&sysblk.scrlock);
    initialize_condition (&sysblk.scrcond);
    initialize_lock (&sysblk.crwlock);
    initialize_lock (&sysblk.ioqlock);
    initialize_condition (&sysblk.ioqcond);
    initialize_lock( &sysblk.dasdcache_lock );

#if defined( _FEATURE_076_MSA_EXTENSION_FACILITY_3 )
    /* Initialize the wrapping key registers lock */
    initialize_rwlock( &sysblk.wklock );
#endif

    /* Initialize thread creation attributes so all of hercules
       can use them at any time when they need to create_thread
    */
    initialize_detach_attr (DETACHED);
    initialize_join_attr   (JOINABLE);

    initialize_condition (&sysblk.cpucond);
    {
        int i;
        for (i = 0; i < MAX_CPU_ENGINES; i++)
            initialize_lock (&sysblk.cpulock[i]);
    }
    initialize_condition (&sysblk.sync_cond);
    initialize_condition (&sysblk.sync_bc_cond);

    /* Copy length for regs */
    sysblk.regs_copy_len = (int)((uintptr_t)&sysblk.dummyregs.regs_copy_end
                               - (uintptr_t)&sysblk.dummyregs);

    /* Set the daemon_mode flag indicating whether we running in
       background/daemon mode or not (meaning both stdout/stderr
       are redirected to a non-tty device). Note that this flag
       needs to be set before logger_init gets called since the
       logger_logfile_write function relies on its setting.
    */
    if (!isatty(STDERR_FILENO) && !isatty(STDOUT_FILENO))
        sysblk.daemon_mode = 1;       /* Leave -d intact */

    /* Initialize the logmsg pipe and associated logger thread.
       This causes all subsequent logmsg's to be redirected to
       the logger facility for handling by virtue of stdout/stderr
       being redirected to the logger facility.

       NOTE that the logger facility must ALWAYS be initialized
       since other threads depend on it to be able to retrieve
       log messages. This is true regardless of whether or not
       the log messages are being redirected to a hardcopy file.

       The HAO thread (Hercules Automatic Operator) for example
       becomes essentially useless if it's unable to retrieve
       log messages to see whether they match any of its rules.
    */
    logger_init();

    /*
       Setup the initial codepage
    */
    set_codepage(NULL);

    /* Initialize default HDL modules load directory */
#if defined( OPTION_DYNAMIC_LOAD )
    hdl_initpath( NULL );
#endif

    /* Process command-line arguments. Exit if any serious errors. */
    if ((rc = process_args( argc, argv )) != 0)
    {
        // "Terminating due to %d argument errors"
        WRMSG( HHC02343, "S", rc );
        usleep( 250000 );   // give logger time to display message(s)
        exit( rc );
    }

    /* Now display the version information again after logger_init
       has been called so that either the panel display thread or the
       external gui can see the version which was previously possibly
       only displayed to the actual physical screen the first time we
       did it further above (depending on whether we're running in
       daemon_mode (external gui mode) or not). This is the call that
       the panel thread or external gui actually "sees".

       The first call further above wasn't seen by either since it
       was issued before logger_init was called and thus got written
       directly to the physical screen whereas this one will be inter-
       cepted and handled by the logger facility thereby allowing the
       panel thread or external gui to "see" it and thus display it.
    */
    display_version       ( stdout, 0, "Hercules" );
    display_build_options ( stdout, 0 );
    display_extpkg_vers   ( stdout, 0 );

    /* Report whether Hercules is running in "elevated" mode or not */
#if defined( _MSVC_ ) // (remove this test once non-Windows version of "is_elevated()" is coded)
    // HHC00018 "Hercules is %srunning in elevated mode"
    if (is_elevated())
        WRMSG( HHC00018, "I", "" );
    else
        WRMSG( HHC00018, "W", "NOT " );
#endif // defined( _MSVC_ )

#if !defined(WIN32) && !defined(HAVE_STRERROR_R)
    strerror_r_init();
#endif

#if defined(OPTION_SCSI_TAPE)
    initialize_lock      ( &sysblk.stape_lock         );
    initialize_condition ( &sysblk.stape_getstat_cond );
    InitializeListHead   ( &sysblk.stape_mount_link   );
    InitializeListHead   ( &sysblk.stape_status_link  );
#endif /* defined(OPTION_SCSI_TAPE) */

    if (sysblk.scrtest)
    {
        // "Hercules is running in test mode."
        WRMSG (HHC00019, "W" );
        if (sysblk.scrfactor != 1.0)
            // "Test timeout factor = %3.1f"
            WRMSG( HHC00021, "I", sysblk.scrfactor );
    }

    /* Set default TCP keepalive values */

#if !defined( HAVE_BASIC_KEEPALIVE )

    WARNING("TCP keepalive headers not found; check configure.ac")
    WARNING("TCP keepalive support will NOT be generated")
    // "This build of Hercules does not support TCP keepalive"
    WRMSG( HHC02321, "E" );

#else // basic, partial or full: must attempt setting keepalive

  #if !defined( HAVE_FULL_KEEPALIVE ) && !defined( HAVE_PARTIAL_KEEPALIVE )

    WARNING("This build of Hercules will only have basic TCP keepalive support")
    // "This build of Hercules has only basic TCP keepalive support"
    WRMSG( HHC02322, "W" );

  #elif !defined( HAVE_FULL_KEEPALIVE )

    WARNING("This build of Hercules will only have partial TCP keepalive support")
    // "This build of Hercules has only partial TCP keepalive support"
    WRMSG( HHC02323, "W" );

  #endif // (basic or partial)

    /*
    **  Note: we need to try setting them to our desired values first
    **  and then retrieve the set values afterwards to detect systems
    **  which do not allow some values to be changed to ensure SYSBLK
    **  gets initialized with proper working default values.
    */
    {
        int rc, sfd, idle, intv, cnt;

        /* Need temporary socket for setting/getting */
        sfd = socket( AF_INET, SOCK_STREAM, 0 );
        if (sfd < 0)
        {
            WRMSG( HHC02219, "E", "socket()", strerror( HSO_errno ));
            idle = 0;
            intv = 0;
            cnt  = 0;
        }
        else
        {
            idle = KEEPALIVE_IDLE_TIME;
            intv = KEEPALIVE_PROBE_INTERVAL;
            cnt  = KEEPALIVE_PROBE_COUNT;

            /* First, try setting the desired values */
            rc = set_socket_keepalive( sfd, idle, intv, cnt );

            if (rc < 0)
            {
                WRMSG( HHC02219, "E", "set_socket_keepalive()", strerror( HSO_errno ));
                idle = 0;
                intv = 0;
                cnt  = 0;
            }
            else
            {
                /* Report partial success */
                if (rc > 0)
                {
                    // "Not all TCP keepalive settings honored"
                    WRMSG( HHC02320, "W" );
                }

                sysblk.kaidle = idle;
                sysblk.kaintv = intv;
                sysblk.kacnt  = cnt;

                /* Retrieve current values from system */
                if (get_socket_keepalive( sfd, &idle, &intv, &cnt ) < 0)
                    WRMSG( HHC02219, "E", "get_socket_keepalive()", strerror( HSO_errno ));
            }
            close_socket( sfd );
        }

        /* Initialize SYSBLK with default values */
        sysblk.kaidle = idle;
        sysblk.kaintv = intv;
        sysblk.kacnt  = cnt;
    }

#endif // (KEEPALIVE)

    /* Initialize runtime opcode tables */
    init_opcode_tables();

#if defined(OPTION_DYNAMIC_LOAD)
    /* Initialize the hercules dynamic loader */
    hdl_main();

    /* Load modules requested at startup */
    if (dll_count >= 0)
    {
        int hl_err = FALSE;
        for ( dll_count = 0; dll_count < MAX_DLL_TO_LOAD; dll_count++ )
        {
            if (dll_load[dll_count] != NULL)
            {
                if (hdl_load(dll_load[dll_count], HDL_LOAD_DEFAULT) != 0)
                {
                    hl_err = TRUE;
                }
                free(dll_load[dll_count]);
            }
            else
                break;
        }

        if (hl_err)
        {
            usleep(10000);      // give logger time to issue error message
            WRMSG(HHC01408, "S");
            delayed_exit(-1);
            return(1);
        }

    }
#endif /* defined(OPTION_DYNAMIC_LOAD) */

#if defined( EXTERNALGUI ) && defined( OPTION_DYNAMIC_LOAD )
    /* Load DYNGUI module if needed */
    if (extgui)
    {
        if (hdl_load("dyngui",HDL_LOAD_DEFAULT) != 0)
        {
            usleep(10000); /* (give logger thread time to issue
                               preceding HHC01516E message) */
            WRMSG(HHC01409, "S");
            delayed_exit(-1);
            return(1);
        }
    }
#endif /* defined( EXTERNALGUI ) && defined( OPTION_DYNAMIC_LOAD ) */

    /* Register the SIGINT handler */
    if ( signal (SIGINT, sigint_handler) == SIG_ERR )
    {
        WRMSG(HHC01410, "S", "SIGINT", strerror(errno));
        delayed_exit(-1);
        return(1);
    }

    /* Register the SIGTERM handler */
    if ( signal (SIGTERM, sigterm_handler) == SIG_ERR )
    {
        // "Cannot register %s handler: %s"
        WRMSG(HHC01410, "S", "SIGTERM", strerror(errno));
        delayed_exit(-1);
        return(1);
    }

#if defined( _MSVC_ )
    /* Register the Window console ctrl handlers */
    if (!IsDebuggerPresent())
    {
        TID dummy;
        BOOL bSuccess = FALSE;

        if (!SetConsoleCtrlHandler( console_ctrl_handler, TRUE ))
        {
            // "Cannot register %s handler: %s"
            WRMSG( HHC01410, "S", "Console-ctrl", strerror( errno ));
            delayed_exit(-1);
            return(1);
        }

        g_hWndEvt = CreateEvent( NULL, TRUE, FALSE, NULL );

        if ((rc = create_thread( &dummy, DETACHED,
            WinMsgThread, (void*) &bSuccess, "WinMsgThread" )) != 0)
        {
            // "Error in function create_thread(): %s"
            WRMSG( HHC00102, "E", strerror( rc ));
            CloseHandle( g_hWndEvt );
            delayed_exit(-1);
            return(1);
        }

        // (wait for thread to create window)
        WaitForSingleObject( g_hWndEvt, INFINITE );
        CloseHandle( g_hWndEvt );

        // Was message window successfully created?

        if (!g_hMsgWnd)
        {
            // "Error in function %s: %s"
            WRMSG( HHC00136, "E", "WinMsgThread", "CreateWindowEx() failed");
            delayed_exit(-1);
            return(1);
        }
    }
#endif

#if defined(HAVE_DECL_SIGPIPE) && HAVE_DECL_SIGPIPE
    /* Ignore the SIGPIPE signal, otherwise Hercules may terminate with
       Broken Pipe error if the printer driver writes to a closed pipe */
    if ( signal (SIGPIPE, SIG_IGN) == SIG_ERR )
    {
        WRMSG(HHC01411, "E", strerror(errno));
    }
#endif

    {
        int fds[2];
        initialize_lock(&sysblk.cnslpipe_lock);
        initialize_lock(&sysblk.sockpipe_lock);
        sysblk.cnslpipe_flag=0;
        sysblk.sockpipe_flag=0;
        VERIFY( create_pipe(fds) >= 0 );
        sysblk.cnslwpipe=fds[1];
        sysblk.cnslrpipe=fds[0];
        VERIFY( create_pipe(fds) >= 0 );
        sysblk.sockwpipe=fds[1];
        sysblk.sockrpipe=fds[0];
    }

#if !defined(NO_SIGABEND_HANDLER)
    {
    struct sigaction sa;
        sa.sa_sigaction = (void*)&sigabend_handler;
#ifdef SA_NODEFER
        sa.sa_flags = SA_NODEFER;
#else
        sa.sa_flags = 0;
#endif

        /* Explictily initialize sa_mask to its default.        @PJJ */
        sigemptyset(&sa.sa_mask);

        if( sigaction(SIGILL, &sa, NULL)
         || sigaction(SIGFPE, &sa, NULL)
         || sigaction(SIGSEGV, &sa, NULL)
         || sigaction(SIGBUS, &sa, NULL)
         || sigaction(SIGUSR1, &sa, NULL)
         || sigaction(SIGUSR2, &sa, NULL) )
        {
            WRMSG(HHC01410, "S", "SIGILL/FPE/SEGV/BUS/USR", strerror(errno));
            delayed_exit(-1);
            return(1);
        }
    }
#endif /*!defined(NO_SIGABEND_HANDLER)*/

#ifdef HAVE_REXX
    /* Initialize Rexx */
    InitializeRexx();
#endif

    /* System initialisation time */
    sysblk.todstart = hw_clock() << 8;

#if !defined(NO_SIGABEND_HANDLER)
    /* Start the watchdog */
    rc = create_thread (&sysblk.wdtid, DETACHED,
                        watchdog_thread, NULL, "watchdog_thread");
    if (rc)
    {
        WRMSG(HHC00102, "E", strerror(rc));
        delayed_exit(-1);
        return(1);
    }
#endif /*!defined(NO_SIGABEND_HANDLER)*/

    hdl_adsc("release_config", release_config, NULL);

    /* Build system configuration */
    if ( build_config (cfgorrc[want_cfg].filename) )
    {
        delayed_exit(-1);
        return(1);
    }

    /* Process the .rc file synchronously when in daemon mode. */
    /* Otherwise Start up the RC file processing thread.       */
    if (cfgorrc[want_rc].filename)
    {
        if (sysblk.daemon_mode)
            process_rc_file( NULL );
        else
        {
            rc = create_thread( &rctid, DETACHED,
                process_rc_file, NULL, "process_rc_file" );
            if (rc)
                WRMSG( HHC00102, "E", strerror( rc ));
        }
    }

    if (log_callback)
    {
        // 'herclin' called us. IT iS in charge. Create its requested
        // logmsg intercept callback function and return back to it.
        rc = create_thread( &logcbtid, DETACHED,
                      log_do_callback, NULL, "log_do_callback" );
        if (rc)
            // "Error in function create_thread(): %s"
            WRMSG( HHC00102, "E", strerror( rc ));

        return (rc);
    }

    /* Activate the control panel */
    if (!sysblk.daemon_mode)
        panel_display();  /* Returns only AFTER Hercules is shutdown */
    else
    {
        /* We're in daemon mode... */
#if defined(OPTION_DYNAMIC_LOAD)
        if (daemon_task)
            daemon_task();/* Returns only AFTER Hercules is shutdown */
        else
#endif /* defined(OPTION_DYNAMIC_LOAD) */
        {
            /* daemon mode without any daemon_task */
            process_script_file("-", 1);

            /* We come here only when the user did ctl-d on a tty or */
            /* end  of  file of the standard input.  No quit command */
            /* has  been issued since that (via do_shutdown()) would */
            /* not return.                                           */

            if (sysblk.started_mask)  /* All quiesced?               */
                usleep( 10 * 1000 );  /* Wait for CPUs to stop       */
            quit_cmd(0, NULL, NULL);  /* Then pull the plug          */
        }
    }

    /*
    **  PROGRAMMING NOTE: the following code is only ever reached
    **  if Hercules is run in normal panel mode -OR- when it is run
    **  under the control of an external GUI (i.e. daemon_task).
    */
    ASSERT( sysblk.shutdown );  // (why else would we be here?!)

#ifdef _MSVC_
    SetConsoleCtrlHandler( console_ctrl_handler, FALSE );
    socket_deinit();
#endif
    fflush( stdout );
    usleep( 10000 );
    return 0; /* return back to bootstrap.c */
} /* end function impl */

/*-------------------------------------------------------------------*/
/* Initialize program name                                           */
/*-------------------------------------------------------------------*/
static void init_progname( int argc, char* argv[] )
{
    free( sysblk.hercules_pgmname );
    free( sysblk.hercules_pgmpath );

    if (argc < 1 || !strlen( argv[0] ))
    {
        sysblk.hercules_pgmname = strdup( "hercules" );
        sysblk.hercules_pgmpath = strdup( "" );
    }
    else
    {
        char path[ MAX_PATH ];

#if defined( _MSVC_ )
        GetModuleFileName( NULL, path, _countof( path ));
#else
        STRLCPY( path, argv[0] );
#endif
        sysblk.hercules_pgmname = strdup( basename( path ));
        sysblk.hercules_pgmpath = strdup( dirname( path ));
    }
}

/*-------------------------------------------------------------------*/
/*         process_args - Process command-line arguments             */
/*-------------------------------------------------------------------*/
static int process_args( int argc, char* argv[] )
{
    int  arg_error = 0;                 /* 1=Invalid arguments       */
    int  c = 0;                         /* Next option flag          */

    // Save a copy of the command line before getopt mangles argv[]

    if (argc > 0)
    {
        int i;
        size_t len;

        for (len=0, i=0; i < argc; i++ )
            len += strlen( argv[i] ) + 1;

        sysblk.hercules_cmdline = malloc( len );
        strlcpy( sysblk.hercules_cmdline, argv[0], len );

        for (i=1; i < argc; i++)
        {
            strlcat( sysblk.hercules_cmdline,  " ",    len );
            strlcat( sysblk.hercules_cmdline, argv[i], len );
        }
    }

    opterr = 0; /* We'll print our own error messages thankyouverymuch */

    if (2 <= argc && !strcmp(argv[1], "-?"))
    {
        arg_error++;
        goto error;
    }

    for (; EOF != c ;)
    {
        c =                       /* Work area for getopt */

#if defined( HAVE_GETOPT_LONG )
            getopt_long( argc, argv, shortopts, longopts, NULL );
#else
            getopt( argc, argv, shortopts );
#endif

        switch (c)
        {
            case EOF:

                break;      /* (we're done) */

            case 0:         /* getopt_long() set a variable; keep going */

                break;

            case 'h':       /* -h[=type] or --help[=type] */

                if (optarg) /* help type specified? */
                {
                    if (0
                        || strcasecmp( optarg, "short"  ) == 0
                    )
                    {
                        ;   // (do nothing)
                    }
                    else if (0
                        || strcasecmp( optarg, "version" ) == 0
                    )
                    {
                        display_version( stdout, 0, "Hercules" );
                    }
                    else if (0
                        || strcasecmp( optarg, "build"   ) == 0
                    )
                    {
                        display_build_options( stdout, 0 );
                    }
                    else if (0
                        || strcasecmp( optarg, "all"  ) == 0
                        || strcasecmp( optarg, "long" ) == 0
                        || strcasecmp( optarg, "full" ) == 0
                    )
                    {
                        display_version( stdout, 0, "Hercules" );
                        display_build_options( stdout, 0 );
                        display_extpkg_vers  ( stdout, 0 );
                    }
                    else
                    {
                        // "Invalid help option argument: %s"
                        WRMSG( HHC00025, "E", optarg );
                    }
                }

                arg_error++;  // (forced by help option)
                break;

            case 'f':

                cfgorrc[ want_cfg ].filename = optarg;
                break;

            case 'r':

                cfgorrc[ want_rc ].filename = optarg;
                break;

#if defined( ENABLE_BUILTIN_SYMBOLS )

            case 's':
            {
                char* sym        = NULL;
                char* value      = NULL;
                char* strtok_str = NULL;

                if (strlen( optarg ) >= 3)
                {
                    sym   = strtok_r( optarg, "=", &strtok_str);
                    value = strtok_r( NULL,   "=", &strtok_str);

                    if (sym && value)
                    {
                        int j;

                        for (j=0; j < (int) strlen( sym ); j++)
                        {
                            if (islower( sym[j] ))
                                sym[j] = toupper( sym[j] );
                        }

                        set_symbol( sym, value );
                    }
                    else
                        // "Symbol and/or Value is invalid; ignored"
                        WRMSG( HHC01419, "E" );
                }
                else
                    // "Symbol and/or Value is invalid; ignored"
                    WRMSG( HHC01419, "E" );
            }
            break;
#endif

#if defined( OPTION_DYNAMIC_LOAD )

            case 'p':

                hdl_initpath( optarg );
                break;

            case 'l':
            {
                char *dllname, *strtok_str = NULL;

                for(dllname = strtok_r(optarg,", ",&strtok_str);
                    dllname;
                    dllname = strtok_r(NULL,", ",&strtok_str))
                {
                    if (dll_count < MAX_DLL_TO_LOAD - 1)
                        dll_load[ ++dll_count ] = strdup( dllname );
                    else
                    {
                        // "Startup parm -l: maximum loadable modules %d exceeded; remainder not loaded"
                        WRMSG( HHC01406, "W", MAX_DLL_TO_LOAD );
                        break;
                    }
                }
            }
            break;

#endif // defined( OPTION_DYNAMIC_LOAD )

            case 'b':

                sysblk.logofile = optarg;
                break;

            case 'v':

                sysblk.msglvl |= MLVL_VERBOSE;
                break;

            case 'd':

                sysblk.daemon_mode = 1;
                break;

#if defined( EXTERNALGUI )

            case 'e':

                extgui = 1;
                break;
#endif

            case 't':

                sysblk.scrtest = 1;
                sysblk.scrfactor = 1.0;

                if (optarg)
                {
                    double scrfactor;
                    double max_factor = MAX_RUNTEST_FACTOR;

                    /* Round down to nearest 10th of a second */
                    max_factor = floor( max_factor * 10.0 ) / 10.0;
                    scrfactor = atof( optarg );

                    if (scrfactor >= 1.0 && scrfactor <= max_factor)
                        sysblk.scrfactor = scrfactor;
                    else
                    {
                        // "Test timeout factor %s outside of valid range 1.0 to %3.1f"
                        WRMSG( HHC00020, "S", optarg, max_factor );
                        arg_error++;
                    }
                }
                break;

            default:
            {
                char buf[16];

                if (isprint( optopt ))
                    MSGBUF( buf, "'-%c'", optopt );
                else
                    MSGBUF( buf, "(hex %2.2x)", optopt );

                // "Invalid/unsupported option: %s"
                WRMSG( HHC00023, "S", buf );
                arg_error++;
            }
            break;

        } // end switch(c)
    } // end while

    while (optind < argc)
    {
        // "Unrecognized option: %s"
        WRMSG( HHC00024, "S", argv[ optind++ ]);
        arg_error++;
    }

error:

    /* Terminate if invalid arguments were detected */
    if (arg_error)
    {
        char   pgm[ MAX_PATH ];
        char*  strtok_str = NULL;

        const char symsub[] =

#if defined( ENABLE_BUILTIN_SYMBOLS )
            " [-s sym=val]";
#else
            "";
#endif
        const char dlsub[] =

#if defined( OPTION_DYNAMIC_LOAD )
            " [-p dyn-load-dir] [[-l dynmod-to-load]...]";
#else
            "";
#endif

        /* Show them all of our command-line arguments... */
        STRLCPY( pgm, sysblk.hercules_pgmname );

        // "Usage: %s [--help[=SHORT|LONG]] [-f config-filename] [-r rcfile-name] [-d] [-b logo-filename]%s [-t [factor]]%s [> logfile]"
        WRMSG( HHC01407, "S", strtok_r( pgm, ".", &strtok_str ), symsub, dlsub );
    }
    else /* Check for config and rc file, but don't open */
    {
        struct stat st;
        int i, rv;

        for (i=0; cfgorrccount > i; i++)
        {
            if (!cfgorrc[i].filename)       /* No value specified */
                cfgorrc[i].filename = getenv( cfgorrc[i].envname );

            if (!cfgorrc[i].filename)       /* No environment var */
            {
                if (!(rv = stat( cfgorrc[i].defaultfile, &st )))
                    cfgorrc[i].filename = cfgorrc[i].defaultfile;
                continue;
            }

            if (0
                || !cfgorrc[i].filename[0]     /* Null name */
                || !strcasecmp( cfgorrc[i].filename, "None" )
            )
            {
               cfgorrc[i].filename = NULL;  /* Suppress file */
               continue;
            }

            /* File specified explicitly or by environment */
            if (-1 == (rv = stat(cfgorrc[i].filename, &st)))
            {
                // "%s file %s not found:  %s"
                WRMSG( HHC02342, "S", cfgorrc[i].whatfile,
                    cfgorrc[i].filename, strerror( errno ));
                arg_error++;
            }
        }
    }

    fflush( stderr );
    fflush( stdout );

    return arg_error;
}

/*-------------------------------------------------------------------*/
/* System cleanup                                                    */
/*-------------------------------------------------------------------*/
DLL_EXPORT void system_cleanup()
{
    /*
        Currently only called by hdlmain,c's HDL_FINAL_SECTION
        after the main 'hercules' module has been unloaded, but
        that could change at some time in the future.
    */
}
