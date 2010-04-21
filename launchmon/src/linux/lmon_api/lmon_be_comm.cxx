/*
 * $Header: /usr/gapps/asde/cvs-vault/sdb/launchmon/src/linux/lmon_api/lmon_be_comm.cxx,v 1.5.2.2 2008/02/20 17:37:57 dahn Exp $
 *--------------------------------------------------------------------------------
 * Copyright (c) 2008, Lawrence Livermore National Security, LLC. Produced at 
 * the Lawrence Livermore National Laboratory. Written by Dong H. Ahn <ahn1@llnl.gov>. 
 * LLNL-CODE-409469. All rights reserved.
 *
 * This file is part of LaunchMON. For details, see 
 * https://computing.llnl.gov/?set=resources&page=os_projects
 *
 * Please also read LICENSE.txt -- Our Notice and GNU Lesser General Public License.
 *
 * 
 * This program is free software; you can redistribute it and/or modify it under the 
 * terms of the GNU General Public License (as published by the Free Software 
 * Foundation) version 2.1 dated February 1999.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY 
 * WARRANTY; without even the IMPLIED WARRANTY OF MERCHANTABILITY or 
 * FITNESS FOR A PARTICULAR PURPOSE. See the terms and conditions of the GNU 
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along 
 * with this program; if not, write to the Free Software Foundation, Inc., 59 Temple 
 * Place, Suite 330, Boston, MA 02111-1307 USA
 *--------------------------------------------------------------------------------			
 *
 *
 *  Note:
 *        LMON BE API is designed to leverage any efficient underlying 
 *        communication infrastructure on a platform. This file contains 
 *        the codes that depend upon this aspect.
 *  
 *
 *  Update Log:
 *        Feb  05 2010 DHA: Added pmgr_register_hname to register the local 
 *                          hostname to the PMGR Collective layer. This is to work 
 *                          around the system in which gethostname followed by
 *                          gethostbyname doesn't resolve the hostname into
 *                          an IP. 
 *        Dec  23 2009 DHA: Added explict config.h inclusion 
 *        Dec  16 2008 DHA: Added COBO support
 *        Mar  26 2008 DHA: Added the END_DEBUG message support
 *                          for BlueGene with a debugger protocol version >= 3
 *        Mar  14 2008 DHA: Added PMGR Collective support. 
 *                          The changes are contained in the PMGR_BASED 
 *                          macro.
 *        Feb  09 2008 DHA: Added LLNS Copyright
 *        Jul  25 2007 DHA: bracket MPI dependent codes using
 *                          LMON_BE_MPI_BASED macro.
 *        Mar  16 2007 DHA: Added MPI_Allreduce support
 *        Dec  29 2006 DHA: Created file. Most routines are lifted 
 *                          from lmon_be.cxx
 */

#ifndef HAVE_LAUNCHMON_CONFIG_H
#include "config.h"
#endif

#include <lmon_api/lmon_api_std.h>

#ifndef LINUX_CODE_REQUIRED
#error This source file requires a LINUX OS
#endif

#if HAVE_STDIO_H
# include <cstdio>
#else
# error stdio.h is required
#endif

#if HAVE_STDLIB_H
# include <cstdlib>
#else
# error stdlib.h is required
#endif

#if HAVE_STRING_H
# include <string.h>
#else
# error string.h is required
#endif

#if HAVE_IOSTREAM
# include <iostream>
#else
# error iostream is required
#endif

extern "C" {
#include <limits.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
}

#include "lmon_api/lmon_proctab.h"
#include "lmon_api/lmon_be.h"
#include "lmon_api/lmon_lmonp_msg.h"
#include "lmon_api/lmon_say_msg.hxx"
#include "lmon_be_internal.hxx"

#if RM_BG_MPIRUN
#include "debugger_interface.h"
using namespace DebuggerInterface;
#endif


#if MPI_BASED
extern "C" {
#include <mpi.h>
}
#elif PMGR_BASED
extern "C" {
#include <pmgr_collective_client.h>
}
#elif COBO_BASED
extern "C" {
#include <cobo.h>
}
#endif

static int ICCL_rank      = -1;
static int ICCL_size      = -1;
static int ICCL_global_id = -1;


//////////////////////////////////////////////////////////////////////////////////
//
// LAUNCHMON BACKEND INTERNAL INTERFACE
//
//
//

//! LMON_be_internal_init(int* argc, char*** argv)
/*!

*/
lmon_rc_e 
LMON_be_internal_init ( int* argc, char*** argv, char *myhn )
{
  int rc;
  char **nargv = *argv;
  int n_lmonopt = 0;
  int i;

  /*
   * The following code assumes lmon's options are specified after all other
   * options (e.g., PMGR's ) are given.
   */
  for (i=1; i < (*argc); ++i) {
    if ( (nargv[i][0] == '-') && (nargv[i][1]) ) {
      if ( strncmp (&(nargv[i][2]), "lmonsharedsec=", 14)== 0 )
	{
	   /* exporting a commandline option to an envVar */
           setenv (TOOL_SS_ENV, &(nargv[i][2])+14, 1); 
	   n_lmonopt++;
	}
      else if ( strncmp (&(nargv[i][2]), "lmonsecchk=", 11)== 0 )
	{
	  /* exporting a commandline option to an envVar */
          setenv (TOOL_SCH_ENV, &(nargv[i][2])+11, 1); 
	  n_lmonopt++;
	}
      }
    } /* for */

  (*argc) -= n_lmonopt;
  nargv[(*argc)+0] = NULL;
  char tmpbuf[PATH_MAX];
  for (i=1; i < (n_lmonopt+1); ++i )
    {
      sprintf(tmpbuf, "LMONNOOP%d=%d", i,i);
       /* this region of memory might not be safe to be freed, so we just leak */
       /* and nargv[(*argc_p)+n_pmgropt] must be null */
      nargv[(*argc)+i] = strdup (tmpbuf); 
    }

#if MPI_BASED
  /*
   * with Message Passing Interface 
   */
  if ( ( rc = MPI_Init (argc, argv)) < 0 )
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true,
		   " MPI_Init failed");
      return LMON_EINVAL;
    }
  //
  // register an MPI error handler to change the behavior of 
  // MPI calls such that they return on failure
  //
  MPI_Errhandler_set (MPI_COMM_WORLD, MPI_ERRORS_RETURN);
  MPI_Comm_size (MPI_COMM_WORLD, &ICCL_size);
  MPI_Comm_rank (MPI_COMM_WORLD, &ICCL_rank);
  ICCL_global_id = ICCL_rank;
#elif PMGR_BASED
  /*
   * with PMGR Collective Interface 
   */
  if ( ( rc = pmgr_init (argc,
      			 argv,
			 &ICCL_size,
			 &ICCL_rank,
			 &ICCL_global_id)) 
       != PMGR_SUCCESS )
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true,
		   "pmgr_init failed");
      return LMON_EINVAL;
    }

  if (pmgr_register_hname (myhn) < 0)
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true,
		   "pmgr_register_hname failed");
      return LMON_EINVAL;
    } 
 
  if ( ( rc = pmgr_open () ) != PMGR_SUCCESS ) 
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true,
		   "pmgr_open failed");
      return LMON_EINVAL;
    }

  if (ICCL_rank == -1)  
    pmgr_getmyrank (&ICCL_rank);

  if (ICCL_size == -1)
    pmgr_getmysize (&ICCL_size); 

#elif COBO_BASED
   int j;
   int *portlist = (int *) malloc (COBO_PORT_RANGE * sizeof(int));

   if (portlist == NULL)
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true,
	"malloc returned NULL");

      return LMON_EINVAL;
    }

   for (j=0; j < COBO_PORT_RANGE; ++j)
     portlist[j] = COBO_BEGIN_PORT+j;

   //
   //TODO: session id=10 for now. 
   //
   if ( ( rc = cobo_open (10, portlist, COBO_PORT_RANGE, &ICCL_rank, &ICCL_size) )
       != COBO_SUCCESS )
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true,
	"cobo_open failed");

      return LMON_EINVAL;
    }
#else
  LMON_say_msg(LMON_BE_MSG_PREFIX, true,"no internal communication fabric to leverage");
  return LMON_EINVAL; 
#endif

  if ( (ICCL_rank == -1) || (ICCL_size == -1 ) )
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true,
	"ICCL_rank and/or ICCL_size have not been assigned");

      return LMON_EINVAL;
    }

  return LMON_OK;
}


lmon_rc_e
LMON_be_internal_getConnFd ( int *fd )
{
#if PMGR_BASED
  if ( pmgr_getsockfd (fd) != PMGR_SUCCESS )
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, false,"no connection estabilished with FE");
      return LMON_EDUNAV;
    }

  return LMON_OK;
#elif COBO_BASED
  if ( ICCL_rank == 0)
    {
      if ( cobo_get_parent_socket (fd) != COBO_SUCCESS )
        {
          LMON_say_msg(LMON_BE_MSG_PREFIX, false,"no connection estabilished with FE");
          return LMON_EDUNAV;
        }
      return LMON_OK;
    }
  else
    {
      return LMON_EDUNAV;
    }
#else
  return LMON_EDUNAV;
#endif
}


//! lmon_rc_e LMON_be_internal_getMyRank
/*
  Returns my rank thru rank argument. 
        Returns: LMON_OK if OK, LMON_EINVAL on error
*/
lmon_rc_e
LMON_be_internal_getMyRank ( int *rank )
{
  if ( ICCL_rank < 0 )
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true,
	"ICCL_rank has not been filled." );

      return LMON_EINVAL;
    }

  (*rank) = ICCL_rank; 

  return LMON_OK;
}


//! lmon_rc_e LMON_be_internal_getSize
/*   
  Returns the number of backend daemons thru size argument.
        Returns: LMON_OK if OK, LMON_EINVAL on error
*/
lmon_rc_e 
LMON_be_internal_getSize ( int *size )
{
  if ( ICCL_size < 0 )
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true, 
	"ICCL_size has not been filled." );

      return LMON_EINVAL;
    }

  (*size) = ICCL_size;

  return LMON_OK;
}


//! lmon_rc_e LMON_be_internal_barrier()
/*
  Barrier call for tool daemons.
*/
lmon_rc_e 
LMON_be_internal_barrier ()
{
  int rc;

#if MPI_BASED
  if ( (rc = MPI_Barrier(MPI_COMM_WORLD)) < 0 )
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true,
        "MPI_Barrier failed ");

      return LMON_EINVAL;
    }
#elif PMGR_BASED
  if ( (rc = pmgr_barrier ()) != PMGR_SUCCESS )
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true,
        "pmgr_barrier failed ");

      return LMON_EINVAL;
    }
#elif COBO_BASED
  if ( (rc = cobo_barrier ()) != COBO_SUCCESS )
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true,
        "pmgr_barrier failed ");

      return LMON_EINVAL;
    }
#else
  LMON_say_msg(LMON_BE_MSG_PREFIX, true,"no internal comm fabric to leverage");
  return LMON_EINVAL; 
#endif
  
  return LMON_OK;
}


//!lmon_rc_e LMON_be_internal_broadcast
/* 
   This call restricts the root of the broadcast call to the master 
   backend daemon and the width of the communicator to be "all daemons"

   This call is also datatype agnostic. It sends buf as a BYTE stream.
 */
lmon_rc_e 
LMON_be_internal_broadcast ( void* buf, int numbyte ) 
{
  int rc;

#if MPI_BASED
  if ( (rc = MPI_Bcast ( buf,
			 numbyte,
			 MPI_BYTE, 
		 	 LMON_BE_MASTER,
			 MPI_COMM_WORLD)) < 0 )
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true," MPI_Bcast failed");

      return LMON_EINVAL;
    }
#elif PMGR_BASED
  if ( (rc = pmgr_bcast ( buf,
			  numbyte,
		 	  LMON_BE_MASTER ))
       != PMGR_SUCCESS )
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true," pmgr_bcast failed");

      return LMON_EINVAL;
    }
#elif COBO_BASED
  if ( (rc = cobo_bcast ( buf,
			  numbyte,
		 	  LMON_BE_MASTER ))
       != COBO_SUCCESS )
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true," cobo_bcast failed");

      return LMON_EINVAL;
    }
#else
  LMON_say_msg(LMON_BE_MSG_PREFIX, true,"no internal comm fabric to leverage");
  return LMON_EINVAL;
#endif

  return LMON_OK;
}


//!lmon_rc_e LMON_be_internal_gather 
/* 
   Gathers data from all tool daemons and returns it via recvbuf 
   of the master tool daemon. The data must be in contiguous memory and
   be constant size across all backend daemons. FIXME:
*/
lmon_rc_e 
LMON_be_internal_gather ( 
	        void *sendbuf,
		int numbyte_per_elem,
		void* recvbuf )
{
  int rc;

#if MPI_BASED
  rc = MPI_Gather (sendbuf,
                   numbyte_per_elem,
                   MPI_BYTE,
                   recvbuf,
                   numbyte_per_elem,
                   MPI_BYTE,
                   LMON_BE_MASTER,
                   MPI_COMM_WORLD);

  if (rc < 0 )
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true,"MPI_Gather failed");

      return LMON_EINVAL;
    }
#elif PMGR_BASED
  rc = pmgr_gather (sendbuf,
	            numbyte_per_elem, 
		    recvbuf,
                    LMON_BE_MASTER);
  if (rc != PMGR_SUCCESS )
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true,"pmgr_gather failed");

      return LMON_EINVAL;
    }
#elif COBO_BASED
  rc = cobo_gather (sendbuf,
	            numbyte_per_elem, 
		    recvbuf,
                    LMON_BE_MASTER);
  if (rc != COBO_SUCCESS )
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true,"cobo_gather failed");

      return LMON_EINVAL;
    }
#else 
  LMON_say_msg(LMON_BE_MSG_PREFIX, true,"no internal comm fabric to leverage");
  return LMON_EINVAL;
#endif

  return LMON_OK;
}


//!lmon_rc_e LMON_be_internal_scatter
/* 
   Scatter data to all tool daemons. Each daemon receives its portion
   via recvbuf. sendbuf is only meaningful to the source.
   The data type of send buf must be in contiguous memory and
   packed so that every daemon receives the constant size per-daemon data.
*/
lmon_rc_e 
LMON_be_internal_scatter (
	        void *sendbuf,
		int numbyte_per_element,
		void* recvbuf )
{
  int rc;

#if MPI_BASED
  rc = MPI_Scatter(sendbuf,
                   numbyte_per_element,
                   MPI_BYTE,
                   recvbuf,
                   numbyte_per_element,
                   MPI_BYTE,
                   LMON_BE_MASTER,
                   MPI_COMM_WORLD);
 
  if (rc < 0 )
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true," MPI_Scatter failed");

      return LMON_EINVAL;
    }
#elif PMGR_BASED
  rc = pmgr_scatter (sendbuf,
                     numbyte_per_element,
                     recvbuf,
                     LMON_BE_MASTER);
 
  if (rc != PMGR_SUCCESS)
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true," pmgr_scatter failed");

      return LMON_EINVAL;
    }
#elif COBO_BASED
  rc = cobo_scatter (sendbuf,
                     numbyte_per_element,
                     recvbuf,
                     LMON_BE_MASTER);

  if (rc != COBO_SUCCESS)
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true," cobo_scatter failed");

      return LMON_EINVAL;
    }
#else
  LMON_say_msg(LMON_BE_MSG_PREFIX, true,"no internal comm fabric to leverage");
  return LMON_EINVAL;
#endif

  return LMON_OK;
}


//! LMON_be_internal_finalize();
/*
  Finalizes the LMON BACKEND API. Every daemon must call this to 
  propertly finalize LMON BACKEND
*/
lmon_rc_e
LMON_be_internal_finalize ()
{
  int rc;

#if RM_BG_MPIRUN
  BG_Debugger_Msg dbgmsg(VERSION_MSG,0,0,0,0);
  BG_Debugger_Msg dbgmsg2(END_DEBUG,0,0,0,0);
  BG_Debugger_Msg ackmsg;
  BG_Debugger_Msg ackmsg2;
  dbgmsg.header.dataLength = sizeof(dbgmsg.dataArea.VERSION_MSG);
  dbgmsg2.header.dataLength = sizeof(dbgmsg2.dataArea.END_DEBUG);

  if ( !BG_Debugger_Msg::writeOnFd (BG_DEBUGGER_WRITE_PIPE, dbgmsg) )
    {
      LMON_say_msg ( LMON_BE_MSG_PREFIX, true,
        "VERSION_MSG command failed.");

      return LMON_EINVAL;
    }
  if ( !BG_Debugger_Msg::readFromFd (BG_DEBUGGER_READ_PIPE, ackmsg) )
    {
      LMON_say_msg ( LMON_BE_MSG_PREFIX, true,
        "VERSION_MSG_ACK failed.");

      return LMON_EINVAL;
    }
  if ( ackmsg.header.messageType != VERSION_MSG_ACK )
    {
      LMON_say_msg ( LMON_BE_MSG_PREFIX, true,
        "readFromFd received a wrong msg type: %d.",
        ackmsg.header.messageType);

      return LMON_EINVAL;
    }
  else 
    {
      if ( ackmsg.dataArea.VERSION_MSG_ACK.protocolVersion >= 3)
        {
# if VERBOSE
          LMON_say_msg ( LMON_BE_MSG_PREFIX, false,
            "BES: debugger protocol higher than or equal to 3.");
# endif

	  if ( !BG_Debugger_Msg::writeOnFd (BG_DEBUGGER_WRITE_PIPE, dbgmsg2) )
            {
      	      LMON_say_msg ( LMON_BE_MSG_PREFIX, true,
        	"END_DEBUG command failed.");

      	      return LMON_EINVAL;
    	    }
  	  if ( !BG_Debugger_Msg::readFromFd (BG_DEBUGGER_READ_PIPE, ackmsg2) )
    	    {
      	      LMON_say_msg ( LMON_BE_MSG_PREFIX, true,
        	"VERSION_MSG_ACK failed.");

 	      return LMON_EINVAL;	
 	    }
	  }
        else
          {
# if VERBOSE
            LMON_say_msg ( LMON_BE_MSG_PREFIX, false,
              "BES: debugger protocol lower than or equal to 3.");
# endif
          }
    }
#endif

#if MPI_BASED
  if ( (rc = MPI_Finalize()) < 0 )
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true," MPI_Finalize failed");

      return LMON_EINVAL;
    }
#elif PMGR_BASED
  if ( ( rc = pmgr_close () ) != PMGR_SUCCESS ) 
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true,
		   "pmgr_close failed");

      return LMON_EINVAL;
    }

  if ( (rc = pmgr_finalize ()) != PMGR_SUCCESS )
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true," pmgr_finalize failed");

      return LMON_EINVAL;
    }
#elif COBO_BASED
  if ( ( rc = cobo_close () ) != COBO_SUCCESS ) 
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true,
        "cobo_close failed");

      return LMON_EINVAL;
    }
#else
  LMON_say_msg(LMON_BE_MSG_PREFIX, true,"no internal comm fabric to leverage");
  return LMON_EINVAL;
#endif

  return LMON_OK;
}


