/*-------------------------------------------------------------------------
 *
 * local_executor.h
 *	Functions and global variables to control local query execution.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef LOCAL_EXECUTION_H
#define LOCAL_EXECUTION_H

#include "distributed/citus_custom_scan.h"

/* enabled with GUCs*/
extern bool EnableLocalExecution;
extern bool LogLocalCommands;

extern bool LocalExecutionHappened;
extern bool TaskAccessesLocalNode(Task *task);

extern uint64 ExecuteLocalTaskList(CitusScanState *scanState, List *taskList);
extern void ExtractLocalAndRemoteTasks(bool readOnlyPlan, List *taskList,
									   List **localTaskList, List **remoteTaskList);
extern bool ShouldExecuteTasksLocally(List *taskList);
extern void ErrorIfLocalExecutionHappened(void);
void SetTaskQueryAndPlacementList(Task *task, Query *query, List *placementList);
extern char * TaskQueryString(Task *task);
extern void DisableLocalExecution(void);
extern bool AnyTaskAccessesRemoteNode(List *taskList);
extern Query * LocalShardQuery(Task *task, ParamListInfo
							   boundParams, int *numParams,
							   Oid **parameterTypes);
#endif /* LOCAL_EXECUTION_H */
