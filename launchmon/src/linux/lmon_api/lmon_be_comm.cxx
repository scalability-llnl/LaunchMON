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

#include <lmon_api/lmon_api_std.h>

#ifndef LINUX_CODE_REQUIRED
#error This source file requires a LINUX OS
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

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
#include <mpi.h>
#elif PMGR_BASED
extern "C" {
#include <pmgr_collective_client.h>
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
LMON_be_internal_init ( int* argc, char*** argv )
{
  int rc;

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

  if ( (ICCL_rank == -1) || (ICCL_size == -1 ) )
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, true,
		   "ICCL_rank and/or ICCL_size have not been assigned");

      return LMON_EINVAL;
    }
#else
  LMON_say_msg(LMON_BE_MSG_PREFIX, true,"no internal communication fabric to leverage");
  return LMON_EINVAL; 
#endif

  return LMON_OK;
}


lmon_rc_e 
LMON_be_internal_getConnFd ( int* fd )
{
#if PMGR_BASED
  if ( pmgr_getsockfd (fd) != PMGR_SUCCESS )
    {
      LMON_say_msg(LMON_BE_MSG_PREFIX, false,"no connection estabilished with FE");
      return LMON_EDUNAV;
    }  
  
  return LMON_OK;
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
LMON_be_internal_getSize ( int* size )
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
      return LMON_EINVAL;
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

#else
  LMON_say_msg(LMON_BE_MSG_PREFIX, true,"no internal comm fabric to leverage");
  return LMON_EINVAL;
#endif

  return LMON_OK;
}


