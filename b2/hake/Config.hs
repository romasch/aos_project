--------------------------------------------------------------------------
-- Copyright (c) 2007-2010, 2012, ETH Zurich.
-- All rights reserved.
--
-- This file is distributed under the terms in the attached LICENSE file.
-- If you do not find this file, copies can be found by writing to:
-- ETH Zurich D-INFK, Haldeneggsteig 4, CH-8092 Zurich. Attn: Systems Group.
--
-- Configuration options for Hake
-- 
--------------------------------------------------------------------------

module Config where

import HakeTypes
import Data.Char
import qualified Args
import Data.List

-- update this variable to change behaviour of the provided code depending on
-- which milestone you're working on.
milestone :: Integer
milestone = 5

-- path to source and install directories; these are automatically set by hake.sh at setup time 
source_dir :: String
-- source_dir = undefined -- (set by hake.sh, see end of file)

install_dir :: String
-- install_dir = undefined -- (set by hake.sh, see end of file)

-- Set of architectures for which to generate rules
architectures :: [String]
-- architectures = undefined -- (set by hake.sh, see end of file)

-- Optimisation flags (-Ox -g etc.) passed to compiler
cOptFlags :: String
cOptFlags = "-g -O2"

-- Selects which libc to compile with, "oldc" or "newlib"
libc :: String
--libc = "oldc"
libc = "newlib"

newlib_malloc :: String
--newlib_malloc = "sbrk"     -- use sbrk and newlib's malloc()
--newlib_malloc = "dlmalloc" -- use dlmalloc
newlib_malloc = "oldmalloc"

-- Use a frame pointer
use_fp :: Bool
use_fp = True

-- Default timeslice duration in milliseconds
timeslice :: Integer
timeslice = 80

-- Put kernel into microbenchmarks mode
microbenchmarks :: Bool
microbenchmarks = False

-- Enable tracing
trace :: Bool
trace = False

-- Enable QEMU networking. (ie. make network work in small memory)
support_qemu_networking :: Bool
support_qemu_networking  = False

-- armv7 platform to build for
-- Currently available: gem5, pandaboard
armv7_platform :: String
-- armv7_platform = "gem5"
armv7_platform = "pandaboard"

-- enable network tracing
trace_network_subsystem :: Bool
trace_network_subsystem = False

-- May want to disable LRPC to improve trace visuals
trace_disable_lrpc :: Bool
trace_disable_lrpc = False

-- use Kaluga
use_kaluga_dvm :: Bool
use_kaluga_dvm = True

-- Domain and driver debugging
global_debug :: Bool
global_debug = False

e1000n_debug :: Bool
e1000n_debug = False

eMAC_debug :: Bool
eMAC_debug = False

rtl8029_debug :: Bool
rtl8029_debug = False

ahcid_debug :: Bool
ahcid_debug = False

libahci_debug :: Bool
libahci_debug = False

vfs_debug :: Bool
vfs_debug = False

ethersrv_debug :: Bool
ethersrv_debug = False

netd_debug :: Bool
netd_debug = False

libacpi_debug :: Bool 
libacpi_debug = False

acpi_interface_debug :: Bool
acpi_interface_debug = False

lpc_timer_debug :: Bool
lpc_timer_debug = False

lwip_debug :: Bool
lwip_debug = False

libpci_debug :: Bool
libpci_debug = False

usrpci_debug :: Bool
usrpci_debug = False

timer_debug :: Bool
timer_debug = False

eclipse_kernel_debug :: Bool
eclipse_kernel_debug = False

skb_debug :: Bool
skb_debug = False

skb_client_debug :: Bool
skb_client_debug = False

flounder_debug :: Bool
flounder_debug = False

flounder_failed_debug :: Bool
flounder_failed_debug = False

webserver_debug :: Bool
webserver_debug = False

sqlclient_debug :: Bool
sqlclient_debug = False

sqlite_debug :: Bool
sqlite_debug = False

sqlite_backend_debug :: Bool
sqlite_backend_debug = False

nfs_debug :: Bool
nfs_debug = False

rpc_debug :: Bool
rpc_debug = False

loopback_debug :: Bool
loopback_debug = False

octopus_debug :: Bool
octopus_debug = False

-- Deadlock debugging
debug_deadlocks :: Bool
debug_deadlocks = False

-- Partitioned memory server
memserv_percore :: Bool
memserv_percore = False

-- Lazy THC implementation (requires use_fp = True)
lazy_thc :: Bool
lazy_thc | elem "armv7" architectures  = False
         | otherwise                   = True

-- Select remote cap database implementation
data Rcap_db = RCAP_DB_NULL | RCAP_DB_CENTRAL | RCAP_DB_TWOPC deriving (Show,Eq)
rcap_db :: Rcap_db
rcap_db = RCAP_DB_NULL

-- Select scheduler
data Scheduler = RBED | RR deriving (Show,Eq)
scheduler :: Scheduler
scheduler = RBED

-- Physical Address Extensions (PAE)-enabled paging on x86-32
pae_paging :: Bool
pae_paging = False

-- Page Size Extensions (PSE)-enabled paging on x86-32
-- Always enabled when pae_paging == True, regardless of value
pse_paging :: Bool
pse_paging = False

-- No Execute Extensions (NXE)-enabled paging on x86-32
-- May not be True when pae_paging == False
nxe_paging :: Bool
nxe_paging = False

oneshot_timer :: Bool
oneshot_timer = False

defines :: [RuleToken]
defines = [ Str ("-D" ++ d) | d <- [  
             if microbenchmarks then "CONFIG_MICROBENCHMARKS" else "",
             if trace then "CONFIG_TRACE" else "",
             if support_qemu_networking then "CONFIG_QEMU_NETWORK" else "",
             if trace_network_subsystem then "NETWORK_STACK_TRACE" else "",
             if trace_disable_lrpc then "TRACE_DISABLE_LRPC" else "",
             if global_debug then "GLOBAL_DEBUG" else "",
             if e1000n_debug then "E1000N_SERVICE_DEBUG" else "",
             if ahcid_debug then "AHCI_SERVICE_DEBUG" else "",
             if libahci_debug then "AHCI_LIB_DEBUG" else "",
             if vfs_debug then "VFS_DEBUG" else "",
             if eMAC_debug then "EMAC_SERVICE_DEBUG" else "",
             if rtl8029_debug then "RTL8029_SERVICE_DEBUG" else "",
             if ethersrv_debug then "ETHERSRV_SERVICE_DEBUG" else "",
             if netd_debug then "NETD_SERVICE_DEBUG" else "",
             if libacpi_debug then "ACPI_DEBUG_OUTPUT" else "",
             if acpi_interface_debug then "ACPI_BF_DEBUG" else "",
             if lpc_timer_debug then "LPC_TIMER_DEBUG" else "",
             if lwip_debug then "LWIP_BARRELFISH_DEBUG" else "",
             if libpci_debug then "PCI_CLIENT_DEBUG" else "",
             if usrpci_debug then "PCI_SERVICE_DEBUG" else "",
             if timer_debug then "TIMER_CLIENT_DEBUG" else "",
             if eclipse_kernel_debug then "ECLIPSE_KERNEL_DEBUG" else "",
             if skb_debug then "SKB_SERVICE_DEBUG" else "",
             if skb_client_debug then "SKB_CLIENT_DEBUG" else "",
             if flounder_debug then "FLOUNDER_DEBUG" else "",
             if flounder_failed_debug then "FLOUNDER_FAILED_DEBUG" else "",
             if webserver_debug then "WEBSERVER_DEBUG" else "",
             if sqlclient_debug then "SQL_CLIENT_DEBUG" else "",
             if sqlite_debug then "SQL_SERVICE_DEBUG" else "",
             if sqlite_backend_debug then "SQL_BACKEND_DEBUG" else "",
             if nfs_debug then "NFS_CLIENT_DEBUG" else "",
             if rpc_debug then "RPC_DEBUG" else "",
             if loopback_debug then "LOOPBACK_DEBUG" else "",
             if octopus_debug then "DIST_SERVICE_DEBUG" else "",
             if debug_deadlocks then "CONFIG_DEBUG_DEADLOCKS" else "",
             if memserv_percore then "CONFIG_MEMSERV_PERCORE" else "",
             if lazy_thc then "CONFIG_LAZY_THC" else "",
             if pae_paging then "CONFIG_PAE" else "",
             if pse_paging then "CONFIG_PSE" else "",
             if nxe_paging then "CONFIG_NXE" else "",
             if libc == "oldc" then "CONFIG_OLDC" else "CONFIG_NEWLIB",
             if oneshot_timer then "CONFIG_ONESHOT_TIMER" else "",
             if use_kaluga_dvm then "USE_KALUGA_DVM" else "",
             "MILESTONE="++(show milestone)
             ], d /= "" ]

-- Sets the include path for the libc
libcInc :: String
libcInc = if libc == "oldc" then "/include/oldc"
          else "/lib/newlib/newlib/libc/include"

-- some defines depend on the architecture/compile options
arch_defines :: Options -> [RuleToken]
arch_defines opts
    -- enable config flags for interconnect drivers in use for this arch
    = [ Str ("-D" ++ d)
       | d <- ["CONFIG_INTERCONNECT_DRIVER_" ++ (map toUpper n)
               | n <- optInterconnectDrivers opts]
      ]
    -- enable config flags for flounder backends in use for this arch
    ++ [ Str ("-D" ++ d)
       | d <- ["CONFIG_FLOUNDER_BACKEND_" ++ (map toUpper n)
               | n <- optFlounderBackends opts]
      ]

-- newlib common compile flags (maybe put these in a config.h file?)
newlibAddCFlags :: [String]
newlibAddCFlags = [ "-DPACKAGE_NAME=\"newlib\"" ,
                    "-DPACKAGE_TARNAME=\"newlib\"",
                    "-DPACKAGE_VERSION=\"1.19.0\"",
                    "-DPACKAGE_BUGREPORT=\"\"",
                    "-DPACKAGE_URL=\"\"",
                    "-D_I386MACH_ALLOW_HW_INTERRUPTS",
                    "-DMISSING_SYSCALL_NAMES",
                    "-D_WANT_IO_C99_FORMATS",
                    "-D_COMPILING_NEWLIB",
                    "-D_WANT_IO_LONG_LONG",
                    "-D_WANT_IO_LONG_DOUBLE",
                    "-D_MB_CAPABLE",
                    "-D__BSD_VISIBLE"]

-- Automatically added by hake.sh. Do NOT copy these definitions to the defaults
source_dir = "../m2/"
architectures = [ "armv7" ]
install_dir = "."
