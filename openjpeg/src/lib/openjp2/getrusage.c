/* this source is borrowed from libc ticket #6
   created by Yuri Dario and slightly modified my Silvan Scherrer */

#define INCL_DOS
#include <os2.h>

#include <errno.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <stdio.h>
#include <time.h>
#include <malloc.h>
#include <unistd.h>

//#include "procstat.h"

#define BUFFER_SIZE 64*1024

static ULONG	timer_interval;

int get_proc_state( int pid, struct rusage *usage);

int get_proc_state( int pid, struct rusage *usage)
{
	APIRET		rc;
	char* 		buffer;
	qsPtrRec_t*	qsPtrRec;
	qsPrec_t*	qsPrec;
	qsTrec_t*	qsTrec;
	ULONG		usertime = 0;
	ULONG		systime = 0;
	int		i;
	double		u, s;

	// query OS interval timer
	if (timer_interval == 0)
		DosQuerySysInfo(QSV_TIMER_INTERVAL, QSV_TIMER_INTERVAL,
					   (PVOID)&timer_interval, sizeof(ULONG));
	
	// allocate memory 
	buffer = malloc( BUFFER_SIZE);
	if (buffer == 0)
		return -1;
	
	// get pid process informations (process and threads)
	rc = DosQuerySysState( QS_PROCESS, 0, pid, 0, buffer, BUFFER_SIZE);
	if (rc) {
		free( buffer);
		return -1;
	}
	
	// evaluate list
 	qsPtrRec = buffer;		// global structure
	qsPrec = qsPtrRec->pProcRec;	// process data
	
	qsTrec = qsPrec->pThrdRec; 	// thread data
	// sum u/s time for every thread
	for( i=0; i<qsPrec->cTCB; i++) {
		usertime += qsTrec[i].usertime;
		systime += qsTrec[i].systime;
	}

	// convert timer units into seconds/microseconds
	u = 10.0 * usertime / timer_interval;
	s = 10.0 * systime / timer_interval;
	usage->ru_utime.tv_sec = u;
	usage->ru_utime.tv_usec = (u - usage->ru_utime.tv_sec) * 1000000;
	usage->ru_stime.tv_sec = s;
	usage->ru_stime.tv_usec = (s - usage->ru_stime.tv_sec) * 1000000;

	free(buffer);
	return 0;
}

int getrusage(int who, struct rusage *usage)
{

	// only RUSAGE_SELF is supported!
	if (who != RUSAGE_SELF) {
	   errno = EINVAL;
	   return -1;
	}

	// internal query failed
	if (get_proc_state( getpid(), usage) == -1) {
	   errno = EINVAL;
	   return -1;
	}

	return 0;
}
