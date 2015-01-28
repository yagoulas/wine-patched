/*
 * Configuration parameters shared between Wine server and clients
 *
 * Copyright 2002 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include "wine/port.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif
#include "wine/library.h"

static const char server_config_dir[] = "/.wine";        /* config dir relative to $HOME */
static const char server_root_prefix[] = "/tmp/.wine";   /* prefix for server root dir */
static const char server_dir_prefix[] = "/server-";      /* prefix for server dir */

static char *bindir;
static char *dlldir;
static char *datadir;
static char *config_dir;
static char *server_dir;
static char *build_dir;
static char *user_name;
static char *argv0_name;

#ifdef __GNUC__
static void fatal_error( const char *err, ... )  __attribute__((noreturn,format(printf,1,2)));
static void fatal_perror( const char *err, ... )  __attribute__((noreturn,format(printf,1,2)));
#endif

#if defined(__linux__) || defined(__FreeBSD_kernel__ )
#define EXE_LINK "/proc/self/exe"
#elif defined (__FreeBSD__) || defined(__DragonFly__)
#define EXE_LINK "/proc/curproc/file"
#endif

/* die on a fatal error */
static void fatal_error( const char *err, ... )
{
    va_list args;

    va_start( args, err );
    fprintf( stderr, "wine: " );
    vfprintf( stderr, err, args );
    va_end( args );
    exit(1);
}

/* die on a fatal error */
static void fatal_perror( const char *err, ... )
{
    va_list args;

    va_start( args, err );
    fprintf( stderr, "wine: " );
    vfprintf( stderr, err, args );
    perror( " " );
    va_end( args );
    exit(1);
}

/* malloc wrapper */
static void *xmalloc( size_t size )
{
    void *res;

    if (!size) size = 1;
    if (!(res = malloc( size ))) fatal_error( "virtual memory exhausted\n");
    return res;
}

/* strdup wrapper */
static char *xstrdup( const char *str )
{
    size_t len = strlen(str) + 1;
    char *res = xmalloc( len );
    memcpy( res, str, len );
    return res;
}

/* check if a string ends in a given substring */
static inline int strendswith( const char* str, const char* end )
{
    size_t len = strlen( str );
    size_t tail = strlen( end );
    return len >= tail && !strcmp( str + len - tail, end );
}

/* remove all trailing slashes from a path name */
static inline void remove_trailing_slashes( char *path )
{
    int len = strlen( path );
    while (len > 1 && path[len-1] == '/') path[--len] = 0;
}

/* build a path from the specified dir and name */
static char *build_path( const char *dir, const char *name )
{
    size_t len = strlen(dir);
    char *ret = xmalloc( len + strlen(name) + 2 );

    memcpy( ret, dir, len );
    if (len && ret[len-1] != '/') ret[len++] = '/';
    strcpy( ret + len, name );
    return ret;
}

/* return the directory that contains the library at run-time */
static char *get_runtime_libdir(void)
{
#ifdef HAVE_DLADDR
    Dl_info info;
    char *libdir;

    if (dladdr( get_runtime_libdir, &info ) && info.dli_fname[0] == '/')
    {
        const char *p = strrchr( info.dli_fname, '/' );
        unsigned int len = p - info.dli_fname;
        if (!len) len++;  /* include initial slash */
        libdir = xmalloc( len + 1 );
        memcpy( libdir, info.dli_fname, len );
        libdir[len] = 0;
        return libdir;
    }
#endif /* HAVE_DLADDR */
    return NULL;
}

/* return the directory that contains the main exe at run-time */
static char *get_runtime_exedir(void)
{
#ifdef EXE_LINK
    char *p, *bindir;
    int size;

    for (size = 256; ; size *= 2)
    {
        int ret;
        if (!(bindir = malloc( size ))) return NULL;
        if ((ret = readlink( EXE_LINK, bindir, size )) == -1) break;
        if (ret != size)
        {
            bindir[ret] = 0;
            if (!(p = strrchr( bindir, '/' ))) break;
            if (p == bindir) p++;
            *p = 0;
            return bindir;
        }
        free( bindir );
    }
    free( bindir );
#endif
    return NULL;
}

/* return the base directory from argv0 */
static char *get_runtime_argvdir( const char *argv0 )
{
    char *p, *bindir, *cwd;
    int len, size;

    if (!(p = strrchr( argv0, '/' ))) return NULL;

    len = p - argv0;
    if (!len) len++;  /* include leading slash */

    if (argv0[0] == '/')  /* absolute path */
    {
        bindir = xmalloc( len + 1 );
        memcpy( bindir, argv0, len );
        bindir[len] = 0;
    }
    else
    {
        /* relative path, make it absolute */
        for (size = 256 + len; ; size *= 2)
        {
            if (!(cwd = malloc( size ))) return NULL;
            if (getcwd( cwd, size - len ))
            {
                bindir = cwd;
                cwd += strlen(cwd);
                *cwd++ = '/';
                memcpy( cwd, argv0, len );
                cwd[len] = 0;
                break;
            }
            free( cwd );
            if (errno != ERANGE) return NULL;
        }
    }
    return bindir;
}

/* initialize the server directory value */
static void init_server_dir( dev_t dev, ino_t ino )
{
    char *p, *root;

#ifdef __ANDROID__  /* there's no /tmp dir on Android */
    root = build_path( config_dir, ".wineserver" );
#elif defined(HAVE_GETUID)
    root = xmalloc( sizeof(server_root_prefix) + 12 );
    sprintf( root, "%s-%u", server_root_prefix, getuid() );
#else
    root = xstrdup( server_root_prefix );
#endif

    server_dir = xmalloc( strlen(root) + sizeof(server_dir_prefix) + 2*sizeof(dev) + 2*sizeof(ino) + 2 );
    strcpy( server_dir, root );
    strcat( server_dir, server_dir_prefix );
    p = server_dir + strlen(server_dir);

    if (dev != (unsigned long)dev)
        p += sprintf( p, "%lx%08lx-", (unsigned long)((unsigned long long)dev >> 32), (unsigned long)dev );
    else
        p += sprintf( p, "%lx-", (unsigned long)dev );

    if (ino != (unsigned long)ino)
        sprintf( p, "%lx%08lx", (unsigned long)((unsigned long long)ino >> 32), (unsigned long)ino );
    else
        sprintf( p, "%lx", (unsigned long)ino );
    free( root );
}

/* retrieve the default dll dir */
const char *get_dlldir( const char **default_dlldir, const char **dll_prefix )
{
    *default_dlldir = DLLDIR;
    *dll_prefix = "/" DLLPREFIX;
    return dlldir;
}

/* initialize all the paths values */
static void init_paths(void)
{
    struct stat st;

    const char *home = getenv( "HOME" );
    const char *user = NULL;
    const char *prefix = getenv( "WINEPREFIX" );

#ifdef HAVE_GETPWUID
    char uid_str[32];
    struct passwd *pwd = getpwuid( getuid() );

    if (pwd)
    {
        user = pwd->pw_name;
        if (!home) home = pwd->pw_dir;
    }
    if (!user)
    {
        sprintf( uid_str, "%lu", (unsigned long)getuid() );
        user = uid_str;
    }
#else  /* HAVE_GETPWUID */
    if (!(user = getenv( "USER" )))
        fatal_error( "cannot determine your user name, set the USER environment variable\n" );
#endif  /* HAVE_GETPWUID */
    user_name = xstrdup( user );

    /* build config_dir */

    if (prefix)
    {
        config_dir = xstrdup( prefix );
        remove_trailing_slashes( config_dir );
        if (config_dir[0] != '/')
            fatal_error( "invalid directory %s in WINEPREFIX: not an absolute path\n", prefix );
        if (stat( config_dir, &st ) == -1)
        {
            if (errno == ENOENT) return;  /* will be created later on */
            fatal_perror( "cannot open %s as specified in WINEPREFIX", config_dir );
        }
    }
    else
    {
        if (!home) fatal_error( "could not determine your home directory\n" );
        if (home[0] != '/') fatal_error( "your home directory %s is not an absolute path\n", home );
        config_dir = xmalloc( strlen(home) + sizeof(server_config_dir) );
        strcpy( config_dir, home );
        remove_trailing_slashes( config_dir );
        strcat( config_dir, server_config_dir );
        if (stat( config_dir, &st ) == -1)
        {
            if (errno == ENOENT) return;  /* will be created later on */
            fatal_perror( "cannot open %s", config_dir );
        }
    }
    if (!S_ISDIR(st.st_mode)) fatal_error( "%s is not a directory\n", config_dir );
#ifdef HAVE_GETUID
    if (st.st_uid != getuid()) fatal_error( "%s is not owned by you\n", config_dir );
#endif

    init_server_dir( st.st_dev, st.st_ino );
}

/* check if bindir is valid by checking for wineserver */
static int is_valid_bindir( const char *bindir )
{
    struct stat st;
    char *path = build_path( bindir, "wineserver" );
    int ret = (stat( path, &st ) != -1);
    free( path );
    return ret;
}

/* check if basedir is a valid build dir by checking for wineserver and ntdll */
/* helper for running_from_build_dir */
static inline int is_valid_build_dir( char *basedir, int baselen )
{
    struct stat st;

    strcpy( basedir + baselen, "/server/wineserver" );
    if (stat( basedir, &st ) == -1) return 0;  /* no wineserver found */
    /* check for ntdll too to make sure */
    strcpy( basedir + baselen, "/dlls/ntdll/ntdll.dll.so" );
    if (stat( basedir, &st ) == -1) return 0;  /* no ntdll found */

    basedir[baselen] = 0;
    return 1;
}

/* check if we are running from the build directory */
static char *running_from_build_dir( const char *basedir )
{
    const char *p;
    char *path;

    /* remove last component from basedir */
    p = basedir + strlen(basedir) - 1;
    while (p > basedir && *p == '/') p--;
    while (p > basedir && *p != '/') p--;
    if (p == basedir) return NULL;
    path = xmalloc( p - basedir + sizeof("/dlls/ntdll/ntdll.dll.so") );
    memcpy( path, basedir, p - basedir );

    if (!is_valid_build_dir( path, p - basedir ))
    {
        /* remove another component */
        while (p > basedir && *p == '/') p--;
        while (p > basedir && *p != '/') p--;
        if (p == basedir || !is_valid_build_dir( path, p - basedir ))
        {
            free( path );
            return NULL;
        }
    }
    return path;
}

/* initialize the argv0 path */
void wine_init_argv0_path( const char *argv0 )
{
    const char *basename;
    char *libdir;

    if (!(basename = strrchr( argv0, '/' ))) basename = argv0;
    else basename++;

    bindir = get_runtime_exedir();
    if (bindir && !is_valid_bindir( bindir ))
    {
        build_dir = running_from_build_dir( bindir );
        free( bindir );
        bindir = NULL;
    }

    libdir = get_runtime_libdir();
    if (libdir && !bindir && !build_dir)
    {
        build_dir = running_from_build_dir( libdir );
        if (!build_dir) bindir = build_path( libdir, LIB_TO_BINDIR );
    }

    if (!libdir && !bindir && !build_dir)
    {
        bindir = get_runtime_argvdir( argv0 );
        if (bindir && !is_valid_bindir( bindir ))
        {
            build_dir = running_from_build_dir( bindir );
            free( bindir );
            bindir = NULL;
        }
    }

    if (build_dir)
    {
        argv0_name = build_path( "loader/", basename );
    }
    else
    {
        if (libdir) dlldir = build_path( libdir, LIB_TO_DLLDIR );
        else if (bindir) dlldir = build_path( bindir, BIN_TO_DLLDIR );

        if (bindir) datadir = build_path( bindir, BIN_TO_DATADIR );
        argv0_name = xstrdup( basename );
    }
    free( libdir );
}

/* return the configuration directory ($WINEPREFIX or $HOME/.wine) */
const char *wine_get_config_dir(void)
{
    if (!config_dir) init_paths();
    return config_dir;
}

/* retrieve the wine data dir */
const char *wine_get_data_dir(void)
{
    return datadir;
}

/* retrieve the wine build dir (if we are running from there) */
const char *wine_get_build_dir(void)
{
    return build_dir;
}

const char *wine_libs[] = {
#ifdef SONAME_LIBCAPI20
    SONAME_LIBCAPI20,
#endif
#ifdef SONAME_LIBCUPS
    SONAME_LIBCUPS,
#endif
#ifdef SONAME_LIBCURSES
    SONAME_LIBCURSES,
#endif
#ifdef SONAME_LIBDBUS_1
    SONAME_LIBDBUS_1,
#endif
#ifdef SONAME_LIBFONTCONFIG
    SONAME_LIBFONTCONFIG,
#endif
#ifdef SONAME_LIBGL
    SONAME_LIBGL,
#endif
#ifdef SONAME_LIBGNUTLS
    SONAME_LIBGNUTLS,
#endif
#ifdef SONAME_LIBGSM
    SONAME_LIBGSM,
#endif
#ifdef SONAME_LIBHAL
    SONAME_LIBHAL,
#endif
#ifdef SONAME_LIBJPEG
    SONAME_LIBJPEG,
#endif
#ifdef SONAME_LIBNCURSES
    SONAME_LIBNCURSES,
#endif
#ifdef SONAME_LIBNETAPI
    SONAME_LIBNETAPI,
#endif
#ifdef SONAME_LIBODBC
    SONAME_LIBODBC,
#endif
#ifdef SONAME_LIBOSMESA
    SONAME_LIBOSMESA,
#endif
#ifdef SONAME_LIBPNG
    SONAME_LIBPNG,
#endif
#ifdef SONAME_LIBSANE
    SONAME_LIBSANE,
#endif
#ifdef SONAME_LIBTIFF
    SONAME_LIBTIFF,
#endif
#ifdef SONAME_LIBV4L1
    SONAME_LIBV4L1,
#endif
#ifdef SONAME_LIBXCOMPOSITE
    SONAME_LIBXCOMPOSITE,
#endif
#ifdef SONAME_LIBXCURSOR
    SONAME_LIBXCURSOR,
#endif
#ifdef SONAME_LIBXEXT
    SONAME_LIBXEXT,
#endif
#ifdef SONAME_LIBXI
    SONAME_LIBXI,
#endif
#ifdef SONAME_LIBXINERAMA
    SONAME_LIBXINERAMA,
#endif
#ifdef SONAME_LIBXRANDR
    SONAME_LIBXRANDR,
#endif
#ifdef SONAME_LIBXRENDER
    SONAME_LIBXRENDER,
#endif
#ifdef SONAME_LIBXSLT
    SONAME_LIBXSLT,
#endif
#ifdef SONAME_LIBXXF86VM
    SONAME_LIBXXF86VM,
#endif
    NULL
};

/* return the list of shared libs used by wine */
const char **wine_get_libs(void)
{
    return &wine_libs[0];
}

/* return the full name of the server directory (the one containing the socket) */
const char *wine_get_server_dir(void)
{
    if (!server_dir)
    {
        if (!config_dir) init_paths();
        else
        {
            struct stat st;

            if (stat( config_dir, &st ) == -1)
            {
                if (errno != ENOENT) fatal_error( "cannot stat %s\n", config_dir );
                return NULL;  /* will have to try again once config_dir has been created */
            }
            init_server_dir( st.st_dev, st.st_ino );
        }
    }
    return server_dir;
}

/* return the current user name */
const char *wine_get_user_name(void)
{
    if (!user_name) init_paths();
    return user_name;
}

/* return the standard version string */
const char *wine_get_version(void)
{
    return PACKAGE_VERSION;
}

struct wine_patch {
    const char *name;
    const char *author;
    const char *title;
} wine_patch_data[] = {
    { "Compholio", "Michael Müller", "Add commandline option --check-libs to test if shared libraries are installed." },
    { "Compholio", "Sebastian Lackner", "Add commandline option --patches to show the patch list." },
    { "Compholio", "Sebastian Lackner", "Add winediag message to show warning, that this isn't vanilla wine." },
    { "Compholio", "Sebastian Lackner", "Append '(Compholio)' at the end of the version string." },
    { "Miscellaneous", "Erich E. Hoover", "Appease the blessed version of gcc (4.5) when -Werror is enabled." },
    { "Miscellaneous", "Erich E. Hoover", "wined3d: Silence repeated resource_check_usage FIXME. [rev 2]" },
    { "Miscellaneous", "Sebastian Lackner", "Appease the Archlinux version of gcc (4.9.1) when -Werror is enabled." },
    { "Miscellaneous", "Sebastian Lackner", "kernel32: Silence repeated CompareStringEx FIXME." },
    { "Miscellaneous", "Sebastian Lackner", "usp10: Silence repeated GSUB_apply_ChainContext[Subst|Pos] FIXMEs." },
    { "Miscellaneous", "Sebastian Lackner", "wined3d: Silence repeated wined3d_swapchain_present FIXME." },
    { "Pipelight", "Michael Müller", "Allow changing strict draw ordering through an exported function." },
    { "Pipelight", "Michael Müller", "Decrease minimum SetTimer interval to 5 ms. [rev 2]" },
    { "Pipelight", "Michael Müller", "Indicate direct rendering through OpenGL extension." },
    { "Pipelight", "Sebastian Lackner", "Implement X11DRV_FLUSH_GDI_DISPLAY ExtEscape command." },
    { "atl-IOCS_Property", "Qian Hong", "Store IOCS data in a property instead of GWLP_USERDATA." },
    { "comctl32-ImageList", "Sebastian Lackner", "Fix issue that dragimage in ImageList only works for first four elements." },
    { "comctl32-LoadIconMetric", "Michael Müller", "Implement LoadIconMetric function." },
    { "configure-Absolute_RPATH", "Sebastian Lackner", "Also add the absolute RPATH when linking against libwine." },
    { "configure-Detect_Gnutls", "Sebastian Lackner", "Fix detection of gnutls on Ubuntu 14.10. [rev 3]" },
    { "dbghelp-KdHelp", "Sebastian Lackner", "Don't fill KdHelp structure for usermode applications." },
    { "dsound-Fast_Mixer", "Alexander E. Patrakov", "Add a linear resampler for use with a large number of dsound mixing buffers." },
    { "fonts-Missing_Fonts", "Torsten Kurbad / Erich E. Hoover", "Implement missing fonts expected by Silverlight. [rev 2]" },
    { "imm32-Cross_Thread_Access", "Aric Stewart", "Limit cross thread access to ImmSet* functions." },
    { "iphlpapi-TCP_Table", "Erich E. Hoover", "Implement AllocateAndGetTcpExTableFromStack." },
    { "kernel32-FindFirstFile", "Sebastian Lackner", "Implement FindFirstFileExW level FindExInfoBasic." },
    { "kernel32-GetFinalPathNameByHandle", "Michael Müller", "Implement GetFinalPathNameByHandle in kernel32." },
    { "kernel32-GetSystemTimes", "Louis Lenders / Erich E. Hoover", "Implement GetSystemTimes. [rev 2]" },
    { "kernel32-GetVolumePathName", "Erich E. Hoover", "Implement GetVolumePathName." },
    { "kernel32-Named_Pipe", "Dan Kegel", "Fix for ConnectNamedPort return value in overlapped mode." },
    { "kernel32-SystemFileCacheSize", "Austin English", "Add stub for [Get|Set]SystemFileCacheSize." },
    { "libs-Unicode_Collation", "Dmitry Timoshkov", "Fix comparison of punctuation characters." },
    { "ntdll-DOS_Attributes", "Erich E. Hoover", "Implement DOS hidden/system file attributes" },
    { "ntdll-Dynamic_DST", "Michael Müller", "Add support for Dynamic DST (daylight saving time) information in registry." },
    { "ntdll-Dynamic_DST", "Sebastian Lackner", "Add Dynamic DST exceptions for Israel Standard Time." },
    { "ntdll-Exception", "Sebastian Lackner", "Fix some differences in exception handling behaviour between Wine and Windows." },
    { "ntdll-FD_Cache", "Sebastian Lackner", "Use lockfree implementation for get_cached_fd. [rev 4]" },
    { "ntdll-FileDispositionInformation", "Dmitry Timoshkov / Erich E. Hoover", "Add support for setting file disposition information." },
    { "ntdll-Fix_Alignment", "Michael Müller", "Move NtProtectVirtualMemory and NtCreateSection to separate pages in ntdll on x86." },
    { "ntdll-Fix_Free", "Erich E. Hoover", "Fix unintentional leaks with ntdll internals [rev 2]" },
    { "ntdll-Heap_FreeLists", "Steaphan Greene", "Improve heap allocation performance by using more fine-grained free lists." },
    { "ntdll-Junction_Points", "Erich E. Hoover", "Support for junction points/reparse points." },
    { "ntdll-NtQuerySection", "Dmitry Timoshkov", "Implement NtQuerySection class SectionBasicInformation." },
    { "ntdll-Pipe_SpecialCharacters", "Michael Müller", "Allow special characters in pipe names." },
    { "ntdll-WRITECOPY", "Michael Müller", "Change WRITECOPY memory protection to WRITE on first write. [rev 3]" },
    { "ntdll-loader_EntryPoint", "Sebastian Lackner", "Set ldr.EntryPoint for main executable." },
    { "ntoskrnl-Irp_Status", "Sebastian Lackner", "Handle issues when driver returns two different status codes from dispatcher." },
    { "quartz-MediaSeeking_Positions", "Erich E. Hoover", "Return correct IMediaSeeking stream positions in quartz." },
    { "riched20-IText_Interface", "Jactry Zeng", "Add COM aggregation support for IRichEditOle." },
    { "riched20-IText_Interface", "Jactry Zeng", "Implement IRichEditOle and ITextDocument support for ITextServices." },
    { "riched20-IText_Interface", "Jactry Zeng", "Implement ITextRange/Selection::{GetChar,GetStart,GetEnd,GetDuplicate,Collapse,SetStart,SetEnd}." },
    { "riched20-IText_Interface", "Jactry Zeng", "Implement ITextRange/Selection::{GetText,SetRange,IsEqual,GetStoryLength}." },
    { "riched20-IText_Interface", "Jactry Zeng", "Implement Stubs for ITextFont interface. [rev 2]" },
    { "riched20-IText_Interface", "Jactry Zeng", "Implement Stubs for ITextPara interface. [rev 2]" },
    { "riched20-IText_Interface", "Jactry Zeng", "Implement Stubs for ITextRange interface. [rev 3]" },
    { "server-ACL_Compat", "Erich E. Hoover", "Compatibility patch for old method of storing extended file system attributes. [rev 6]" },
    { "server-Address_Change_Notification", "Erich E. Hoover", "Implement SIO_ADDRESS_LIST_CHANGE. [rev 2]" },
    { "server-CreateProcess_ACLs", "Joris van der Wel / Sebastian Lackner", "Implement passing ACLs to CreateProcess. [rev 2]" },
    { "server-Inherited_ACLs", "Erich E. Hoover", "Add support for inherited security attributes. [rev 7]" },
    { "server-Misc_ACL", "Erich E. Hoover", "Add default security descriptor ownership and DACLs for processes." },
    { "server-OpenProcess", "Michael Müller", "Return error when opening a terminating process. [rev 3]" },
    { "server-Stored_ACLs", "Erich E. Hoover", "Store and return security attributes with extended file attributes. [rev 7]" },
    { "shell32-Default_Folder_ACLs", "Erich E. Hoover", "Generate default ACLs for user shell folders. [rev 6]" },
    { "shell32-Default_Path", "Sebastian Lackner", "Implement KF_FLAG_DEFAULT_PATH flag for SHGetKnownFolderPath." },
    { "shell32-Icons", "Michael Müller", "Add support for extra large and jumbo icon lists in shell32." },
    { "shell32-RunDLL_CallEntry16", "Michael Müller", "Manually relay RunDLL_CallEntry16 to make Tages Protection v5 happy." },
    { "shell32-SHCreateSessionKey", "Dmitry Timoshkov", "shell32: Implement SHCreateSessionKey." },
    { "shlwapi-PathIsDirectoryEmptyW", "Michael Müller", "Correctly treat '.' when enumerating files in PathIsDirectoryEmptyW." },
    { "shlwapi-UrlCombine", "Sebastian Lackner", "Workaround for broken implementation of shlwapi url functions." },
    { "user32-Dialog_Paint_Event", "Sebastian Lackner", "Call UpdateWindow during DIALOG_CreateIndirect." },
    { "user32-GetSystemMetrics", "Michael Müller", "Allow changing the tablet / media center status via wine registry key." },
    { "user32-GetTipText", "Erich E. Hoover", "Handle TOOLTIPS_GetTipText edge cases." },
    { "user32-WndProc", "Sebastian Lackner", "Workaround for programs leaking wndproc splots." },
    { "wine.inf-ProductId", "Yanis Lukes", "wine.inf: Add fake ProductId to HKLM\\\\CurrentVersionNT." },
    { "wineboot-HKEY_DYN_DATA", "Michael Müller", "Add some generic hardware in HKEY_DYN_DATA\\\\Config Manager\\\\Enum." },
    { "winebuild-LinkerVersion", "Michael Müller", "Set a valid major and minor linker version in the PE header." },
    { "wined3d-DXTn", "Michael Müller", "Add support for DXTn software decoding through libxtc_dxtn." },
    { "wined3d-Revert_PixelFormat", "Ken Thomases", "Revert wined3d pixelformat changes." },
    { "winemenubuilder-Desktop_Icon_Path", "Sebastian Lackner", "Create desktop shortcuts with absolute wine path." },
    { "winepulse-PulseAudio_Support", "Maarten Lankhorst", "Winepulse patches extracted from https://launchpad.net/~ubuntu-wine/+archive/ubuntu/ppa/+files/wine1.7_1.7.22-0ubuntu1.debian.tar.gz. [rev 4]" },
    { "winex11-CandidateWindowPos", "Felix Yan", "Update a candidate window's position with over-the-spot style. [rev 2]" },
    { "winex11-Limited_Resolutions", "Erich E. Hoover", "Update the check for broken nVidia RandR to test for the number of resolutions instead of the number of modes." },
    { "winex11-Window_Groups", "Michael Müller", "Prevent window managers from grouping all wine programs together." },
    { "winex11-XEMBED", "Sebastian Lackner", "Enable/disable windows when they are (un)mapped by foreign applications." },
    { "winex11-wglShareLists", "Michael Müller", "Only warn about used contexts in wglShareLists." },
    { "wpcap-Dynamic_Linking", "André Hentschel", "Use dynamic linking for libpcap." },
    { "ws2_32-Connect_Time", "Bruno Jesus / Erich E. Hoover", "Return the appropriate connection time with SO_CONNECT_TIME." },
    { "ws2_32-TransmitFile", "Erich E. Hoover", "Implement TransmitFile." },
    { "ws2_32-inet_pton", "Bruno Jesus", "Implement ws2_32.inet_pton." },
    { "wtsapi32-EnumerateProcesses", "Sebastian Lackner", "Partial implementation of WTSEnumerateProcessesW." },
    { NULL, NULL, NULL }
};

/* return the applied non-standard patches */
const void * wine_get_patches(void)
{
    return &wine_patch_data[0];
}

/* return the build id string */
const char *wine_get_build_id(void)
{
    extern const char wine_build[];
    return wine_build;
}

/* exec a binary using the preloader if requested; helper for wine_exec_wine_binary */
static void preloader_exec( char **argv, int use_preloader )
{
    if (use_preloader)
    {
        static const char preloader[] = "wine-preloader";
        static const char preloader64[] = "wine64-preloader";
        char *p, *full_name;
        char **last_arg = argv, **new_argv;

        if (!(p = strrchr( argv[0], '/' ))) p = argv[0];
        else p++;

        full_name = xmalloc( p - argv[0] + sizeof(preloader64) );
        memcpy( full_name, argv[0], p - argv[0] );
        if (strendswith( p, "64" ))
            memcpy( full_name + (p - argv[0]), preloader64, sizeof(preloader64) );
        else
            memcpy( full_name + (p - argv[0]), preloader, sizeof(preloader) );

        /* make a copy of argv */
        while (*last_arg) last_arg++;
        new_argv = xmalloc( (last_arg - argv + 2) * sizeof(*argv) );
        memcpy( new_argv + 1, argv, (last_arg - argv + 1) * sizeof(*argv) );
        new_argv[0] = full_name;
        execv( full_name, new_argv );
        free( new_argv );
        free( full_name );
    }
    execv( argv[0], argv );
}

/* exec a wine internal binary (either the wine loader or the wine server) */
void wine_exec_wine_binary( const char *name, char **argv, const char *env_var )
{
    const char *path, *pos, *ptr;
    int use_preloader;

    if (!name) name = argv0_name;  /* no name means default loader */

#ifdef linux
    use_preloader = !strendswith( name, "wineserver" );
#else
    use_preloader = 0;
#endif

    if ((ptr = strrchr( name, '/' )))
    {
        /* if we are in build dir and name contains a path, try that */
        if (build_dir)
        {
            argv[0] = build_path( build_dir, name );
            preloader_exec( argv, use_preloader );
            free( argv[0] );
        }
        name = ptr + 1;  /* get rid of path */
    }

    /* first, bin directory from the current libdir or argv0 */
    if (bindir)
    {
        argv[0] = build_path( bindir, name );
        preloader_exec( argv, use_preloader );
        free( argv[0] );
    }

    /* then specified environment variable */
    if (env_var)
    {
        argv[0] = (char *)env_var;
        preloader_exec( argv, use_preloader );
    }

    /* now search in the Unix path */
    if ((path = getenv( "PATH" )))
    {
        argv[0] = xmalloc( strlen(path) + strlen(name) + 2 );
        pos = path;
        for (;;)
        {
            while (*pos == ':') pos++;
            if (!*pos) break;
            if (!(ptr = strchr( pos, ':' ))) ptr = pos + strlen(pos);
            memcpy( argv[0], pos, ptr - pos );
            strcpy( argv[0] + (ptr - pos), "/" );
            strcat( argv[0] + (ptr - pos), name );
            preloader_exec( argv, use_preloader );
            pos = ptr;
        }
        free( argv[0] );
    }

    /* and finally try BINDIR */
    argv[0] = build_path( BINDIR, name );
    preloader_exec( argv, use_preloader );
    free( argv[0] );
}
